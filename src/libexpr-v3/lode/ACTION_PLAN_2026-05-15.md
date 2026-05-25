# v3 Action Plan — 2026-05-15

**Premise.** The honest cross-agent assessment (this date) confirmed: v3 moved sideways for ~2 weeks. Bench unmoved 6+ days. v3-direct fails on real nixpkgs at a new floor every 3 days. 169 env-var gates accumulated. vm.cc at 9 755 LoC. lode/ has 54 design docs for a subsystem that still cannot evaluate `hello.name`. This plan is meta-corrective: it changes **how** the work is structured, not just what's next on the queue.

The goal of this plan is *not* "fix every open bug". It is **break the investigation-without-convergence pattern** and re-establish a measurable, forward-moving cadence.

---

## Part 1 — What we stop doing (effective immediately)

These are bright-line rules. Violations should be called out in review.

**Rule 0 (the meta-rule, from `LESSONS_LEARNED_2026-05-15.md` Part 0)**: Every commit body must answer "what hypothesis does this kill?" If it kills none, it doesn't merge. A commit may exit an investigation by falsifying a model (delete code + gate), confirming a model (delete alternative + its gate), or renaming the investigation to a fresh top-level issue. A commit may NOT exit by adding an opt-in gate so "both can coexist for now," by adding a diagnostic with no kill criterion, by reverting + reapplying without measurement, or by producing a findings doc without code change.

All subsequent rules in this section are specializations of Rule 0.

1. **No new RCA letter / STG number on an open one.** A1-A12 has one root cause class for A1-A7, a different one for A8/A9, a third for A12. If a workload has an open A-letter, the next divergence on it gets a sub-letter on the same root-cause track, not a new top-level investigation. STG-15 may not exist until STG-14b is closed or explicitly retired.

2. **No new `NIX_V3_*` / `V3_DBG_*` gate without an inline retirement criterion.** Required format on the *first line that reads the env var*:
   ```cpp
   // gate: NIX_V3_FOO — purpose. Retire when [observable Y achieved or X closed].
   ```
   PRs that add an ungated env var are reverted, not amended.

3. **No "Phase N follow-up" commits without a measurable victory condition** stated in the commit body. "Theoretically faster" / "lays groundwork" / "should help once X" are not victory conditions. The body must contain a before/after number or "(no perf change expected; correctness only)".

4. **No commit referencing v3_hook.cc except a deletion.** It's gone. Stale comments are dead code in slow motion.

5. **No new lode/ design doc until the previous one is closed.** A doc is closed when (a) its work landed and a follow-up "RESOLVED" line was appended, or (b) it was renamed `*_DEFERRED.md` with a one-line reason. New design docs while five are mid-flight feeds the archaeology pattern.

6. **No `getenv("X")` / `getenv("Y")` / shell-prototype style gates** in main. `test/lint-no-inline-getenv.sh` should fail CI; if it doesn't, fix it before adding the next gate.

---

## Part 2 — Strategic principles

- **Symptoms are not bugs.** Track by root-cause class. Today's three classes are: (a) eager-vs-lazy inherit-from asymmetry, (b) fakeClo recycle/pool protocol, (c) memoization/slot overwrite in rec-attrset access. Every open issue maps to one of these (or reveals a fourth).
- **Bench is the floor, not the ceiling.** If `bench/` hasn't been re-run in 7 days, all perf claims expire. If v3-direct cannot complete `hello.name`, "parity" is an unsupported claim about lang tests + micros only.
- **Reap before adding.** Each landing PR must net-decrease one of: gate count, vm.cc line count, lode/ open-doc count, dead-read references. CI computes the deltas.
- **Decision points are mandatory.** Each phase below has a kill criterion. If the criterion fails, the phase pauses; we do not silently roll into the next sub-phase.
- **Bisect nixpkgs to find the unit you can falsify against.** When a v3-direct failure on real nixpkgs surfaces, do not debug against the full eval — bisect nixpkgs itself (overlays, system, attribute path, by-name slices, commit history, env-gate combinations) until you have a 5-20 line `.nix` reproducer. Save it as `test/repro-<issue>-<shape>.nix` with a `run-<issue>-tests.sh` driver. Keep it as a regression test forever, even after the bug closes — the repro becomes a positive guardrail. Full methodology: `LESSONS_LEARNED_2026-05-15.md` §4.8.

---

## Part 3 — Phases with concrete exit criteria

### Phase 0 — Stop the bleed (Days 1-3, 2026-05-15 → 2026-05-17)

Hygiene only. No new features. No new gates. No new investigations.

- [ ] **Delete `CFF_TAINTED`.** Reader at `vm.cc:3573`, second reader near 4053, zero writers since commit ab3357f2a / 783020080. Delete the flag, the bit, the read sites, and any comments that mention it. (2 h)
- [ ] **Reap v3_hook tombstones.** 12 stale comments in vm.cc / primops.cc / lower.cc / meson.build / run.cc reference the deleted file. Delete each comment or rewrite to current truth. (2 h)
- [ ] **Wire `analyseOccurrence` into `optimise()`** per `OPT_OCCUR_PLAN_2026-05-08.md` Phase B (already specified there). `opt_const_fold.cc:279` is the pipeline driver. Test: one new smoke case where occurrence-info-driven DCE removes a one-shot binding. (1 day)
- [ ] **Make `run-fail-tests.sh` diff `.err.exp` exactly.** Current loose-regex match misses C1-C8 silent semantic gaps. Replace regex with byte-diff + a sanitizer for absolute paths. (4 h)
- [ ] **Add fixtures for C1-C8** (`null && true→true`, missing-required-formal, `"foo"+1`, `//` operand order, `path+"ctx"`, `f==f`, `addErrorContext`, PrimOpApp head). One `.nix` + one `.err.exp` per case. These are not yet fixes — they're failing fixtures that quantify the gap. (1 day)
- [ ] **Re-run the full bench harness against current HEAD.** Commit as `bench/baselines/2026-05-15-action-plan-baseline.json`. This is the floor every subsequent perf claim is measured against. (1 h)
- [ ] **Env-var audit** — one doc, `ENV_VAR_INVENTORY_2026-05-15.md`. Each of the 169 gates: name, file:line, default state, what it does, *retirement criterion*. Three categories: KEEP (≤20 expected), RETIRE-NOW (dead readers, deleted writers, or duplicated polarity), RETIRE-AFTER-X (gated on a specific bug close). (1 day)

**Phase 0 exit criterion**: Env-var inventory complete; bench baseline committed; eval-fail tests diff exactly; C1-C8 fixtures merged (red, not green); `CFF_TAINTED` and v3_hook tombstones gone. **Net: ~150 LoC out, ~5 gates retired, +1 wired optimizer pass, +8 failing fixtures.**

**Kill criterion**: If we cannot complete Phase 0 in 3 days, the codebase has more dead-comment archaeology than estimated; budget Phase 0 to a week before starting Phase 1.

---

### Phase 1.5 — Workload measurement spike (1-2 weeks, post-Phase-1)

Added 2026-05-17 after a verification agent flagged that several candidate roadmap additions (incremental eval / salsa, persistent attrsets / HAMT) rest on **unmeasured** workload-distribution hypotheses. Per Rule 0, a hypothesis must be killed or confirmed by measurement before stage commitment.

**Goal**: produce data that lets us decide whether to commit Stages 10 (salsa-style result cache) and 11 (HAMT attrsets) — or to skip them. See `PERF_STRATEGY_2026-05-17.md`.

**TODOs**:
- [ ] **Instrument `NIX_EVAL_REPEAT_PROFILE`** in v3: per-cell-hash force counts; cross-invocation cell-hash overlap; result-equality fraction.
- [ ] **Measure three workloads, cold + warm**:
  - `(import <nixpkgs> {}).hello.name` (small)
  - cardano-node flake `.packages.<system>.cardano-node` (medium fix-point overlay)
  - `nixos-rebuild dry-build` (large module-eval)
- [ ] **Measure overlay-heavy workload**: haskellPackages with overrides; attrset-size histogram; `//`-update cost fraction.
- [ ] **Add parallel-potential trace analysis** (per `PARALLEL_EVAL_CAPABILITIES_2026-05-18.md` §8): record (forced thunk, parent thunk, force duration) tuples; offline-compute critical path length, total parallel work, theoretical N-core speedup. Decides Stage 13 fate.
- [ ] **NEW: drvPath/outPath force-rate decomposition profile**. After Phase 1 closure, the new floor is `hello.drvPath` / `.outPath` at ~30× slower than TW. Use the same trace infrastructure to decompose the 200× per-force gap into: (a) dispatch cost per opcode, (b) GC scan time per force (Boehm arena watermark progression), (c) intermediate allocation count per logical op, (d) Value-identity-sharing comparison TW vs v3 (does TW share `Value*` across positions where v3 produces fresh Tag::Slot results?). Output drives prioritization between Phase 2 (cycle handling), Stage 3 (nursery), Stage 4 (strictness analysis). Memory reference: `project_force_rate_decomposition_2026-05-18.md`.
  - **Instrument for items (b) and (c)** (added 2026-05-20): `PERF_TRACE_TOOL_DESIGN_2026-05-20.md` specifies a sidecar `psutil` sampler + in-process `GC_get_heap_size()` probe gated by `NIX_V3_HEAP_TRACE`, producing SVG overlays of CPU% / RSS / Boehm-heap over time for TW vs v3 on identical workloads. ~3 days to first measurement. **The first measurement run is the Rule 0 falsifier** for the factor-2 dominance hypothesis (see §"Rule 0 falsification anchor" in that doc). Build the tool before continuing this TODO; otherwise factors (b) and (c) are unobservable.
- [ ] **Produce `MEASUREMENT_SPIKE_2026-XX-XX.md`** with verdict on each candidate stage (10 salsa / 11 HAMT / 12 JIT / 13 multi-core) AND prioritization of Phase 2 / Stage 3 / Stage 4 given the drvPath profile.

**Exit criterion**: a one-page report whose verdict is one of:
- Salsa hypothesis CONFIRMED (>40% warm fraction → commit Stage 10).
- Salsa hypothesis REFUTED (<20% warm → don't commit; PIC + linking are right).
- Salsa hypothesis PARTIAL (commit narrower scope, e.g. flake-output level only).
- Same for HAMT (commit / don't / narrow).

**Kill criterion**: if the spike itself takes >3 weeks, the instrumentation is wrong, not the hypothesis. Re-scope.

**Why this lands here, not later**: measurement comes before commitment, not after. The action plan's Rule 0 prohibits adding stages "to coexist with current plan" based on hypotheses; the spike either kills or confirms before stage commitment.

---

### Phase 1.6 — Resource limit enforcement: `NIX_V3_MAX_HEAP` + `NIX_V3_MAX_CPU_TIME` + `NIX_V3_MAX_WALL_TIME` (4-5 days; landing target 2026-05-22)

Added 2026-05-18; expanded the same day to include CPU + wall-time caps (same category, same implementation pattern). Small enabling-infrastructure piece that unblocks both Phase 1.5 (measurement under controlled conditions) AND the cardano-node feasibility sprint (prevents silent OOM mid-evaluation + bounds runaway eval). Heap design lives in `MAX_HEAP_LIMIT_DESIGN_2026-05-18.md`; time caps follow the same hybrid (in-process poll + optional OS backstop) approach.

**Why this lands before Phase 2 / cardano-node work**: cross-cuts both Pillar 1 (perf) and Pillar 2 (cardano-node). Without a memory cap, Phase 1.5's `hello.drvPath` measurement cannot distinguish "v3 working under normal conditions" from "Boehm growing arbitrarily" — the perf decomposition becomes ambiguous. Cardano-node likely allocates 10-50GB; without a cap, M5 fails as a SIGKILL or swap-thrash rather than a typed exception with diagnostic. The cap is the universal pre-pre-flight that makes the actual pre-flight (200× re-measurement) meaningful.

**TODOs — heap cap**:
- [ ] Implement `GC_set_max_heap_size(N)` + `GC_set_oom_fn(handler)` per `MAX_HEAP_LIMIT_DESIGN_2026-05-18.md` §2. (~1 day)
- [ ] Add `v3::OutOfMemoryError : public EvalError` type with the existing exception-class hierarchy. Hook dispatch-loop catch. (~0.25 day)
- [ ] Env-var parsing for `NIX_V3_MAX_HEAP=2G` / `512M` / `64K` size suffixes. (~0.25 day)
- [ ] Diagnostic snapshot on OOM: top-N allocators (from existing `allocStats` machinery), nursery occupancy, GC counts, current Boehm heap size, actual RSS. (~0.5 day)
- [ ] Optional periodic RSS verification (`NIX_V3_VERIFY_RSS=1`): Linux `/proc/self/status` + macOS `task_info()`. Both as one-syscall-per-N-opcodes polling. (~1 day)

**TODOs — CPU/wall-time caps**:
- [ ] Periodic `getrusage(RUSAGE_SELF)` poll in dispatch loop (every ~10 000 opcodes). Throw `v3::CpuTimeExceededError` if `ru_utime + ru_stime > NIX_V3_MAX_CPU_TIME`. (~0.5 day)
- [ ] Periodic `std::chrono::steady_clock::now()` poll on the same cadence. Throw `v3::WallTimeExceededError` if `now - evalStart > NIX_V3_MAX_WALL_TIME`. (~0.25 day)
- [ ] Optional `setrlimit(RLIMIT_CPU, hardLimit)` backstop — if periodic check misses (e.g., busy syscall), OS sends SIGXCPU at `NIX_V3_MAX_CPU_TIME + 30s` grace; signal handler sets atomic flag; dispatch loop checks flag at next safe point. (~0.25 day)
- [ ] Add `v3::CpuTimeExceededError : public EvalError` and `v3::WallTimeExceededError : public EvalError`. (~0.25 day)
- [ ] Env-var parsing for time suffixes: `60s` / `5m` / `1h`. (~0.25 day)
- [ ] Diagnostic snapshot on time exceed: top-N allocators, recent opcodes, frame stack depth, IFD status (currently in a daemon call?), CPU vs wall ratio. (~0.5 day)

**TODOs — shared**:
- [ ] Each new env var gets Rule 0 retirement-criterion comment at first `getenv()` read site.
- [ ] Bench harness (`bench/bench.py`) gains `--max-heap` + `--cpu-budget` + `--wall-budget` parameters; default `NIX_V3_MAX_HEAP=4G` / `NIX_V3_MAX_CPU_TIME=300` / `NIX_V3_MAX_WALL_TIME=600` for all benchmarks to catch "perf win that doubled memory or runtime" regressions.
- [ ] Test fixtures: (a) synthetic that should OOM (`test/repro-max-heap-oom.nix`); (b) synthetic infinite-loop that should hit CPU cap (`test/repro-max-cpu-busyloop.nix`); (c) synthetic slow-IFD that should hit wall cap (`test/repro-max-wall-slow-ifd.nix`); (d) regression that lang-tests pass under `NIX_V3_MAX_HEAP=512M NIX_V3_MAX_CPU_TIME=60`.

**Exit criteria**:
- `NIX_V3_MAX_HEAP=128M v3-eval --file repro-large.nix` produces typed `OutOfMemoryError` with top-allocator diagnostic.
- `NIX_V3_MAX_CPU_TIME=5 v3-eval --expr '(let f = x: f (x+1); in f 0)'` produces typed `CpuTimeExceededError` within ~5-6 CPU seconds.
- `NIX_V3_MAX_WALL_TIME=10 v3-eval --file repro-slow-ifd.nix` produces typed `WallTimeExceededError` after ~10 wall seconds.
- Lang test suite (142 tests) + cutover-parity (142 tests) both pass under `NIX_V3_MAX_HEAP=512M NIX_V3_MAX_CPU_TIME=300`.
- Bench harness accepts the three new flags and emits caps as part of result metadata.
- `USAGE.md` documents the three env vars.

**Kill criterion**: if Boehm's `GC_set_max_heap_size` doesn't behave as documented on any platform we care about, OR if `getrusage()` polling adds >2% overhead to lang-test runtime, the in-process approach is wrong for that piece; pivot to external `ulimit -v` / `ulimit -t` / `timeout` wrapper. Wrapper is simpler but loses typed-exception diagnostic.

**Rule 0 framing**: this commit kills three hypotheses simultaneously:
- "v3 evaluations are bounded by available system memory in practice" — falsified by `NIX_V3_MAX_HEAP`.
- "v3 evaluations can run forever without bound" — falsified by `NIX_V3_MAX_CPU_TIME`.
- "v3 evaluations can hang on IFD without bound" — falsified by `NIX_V3_MAX_WALL_TIME`.

Each cap gives the user a graceful "fail-fast with diagnostic" instead of SIGKILL / hang / OOM.

---

### Phase 1.7 — Stage 3 readiness: GC correctness gaps + diagnostic-in-CI (5-7 days; landing target 2026-05-28)

Added 2026-05-21 after the two-round GC audit (`RCA_VALUEPAIR_EVALUATED_2026-05-21.md`
+ `GC_AUDIT_ROUND_2_2026-05-21.md`). Round 1 fixed the primary `ValuePair::evaluated`
SIGSEGV; Round 2 surfaced 12 more findings ranging from real-but-narrow SIGSEGV paths
to architectural correctness gaps. Stage 3 (`Nursery default-on`, weeks 15-20 in
ROADMAP) cannot flip the default until these are closed — otherwise it ships with
known SIGSEGV paths.

**Why this lands before Stage 3, not inside it**: Stage 3's existing TODO list assumes
Phase C scavenge is correctness-complete and focuses on Phase D write barriers + Phase E
frequency policy + closure-pool retirement. Round 2 falsifies that assumption: at
least three classes of missed roots remain reachable on default-on workloads. Pushing
them into Stage 3 itself risks the 6-week budget exploding into 10+ weeks of
correctness-and-perf intermixed work. Landing the correctness gaps as a focused
sub-week here keeps Stage 3 itself on its perf-focused budget.

**TODOs — close remaining missed roots (each <1 day)**:
- [ ] **Walk `Thunk::shapeCell`** in `gc.cc:walkThunk` and `Auditor::visitThunk`.
  Currently un-walked; gated by `NIX_V3_CELL_EVERYWHERE=1` so latent today, but
  must close before either Stage 3 default-on or any future flip of CELL_EVERYWHERE.
- [ ] **Forward `CallFrame::forceWriteTarget` pointer when it points into a
  nursery ListVec** (round-1 #6; refined by round-2 N4). The deepForceList path
  at `vm.cc:8930` stores `&list->elems[i]` where `list` may be nursery. Scavenger
  visits `*forceWriteTarget` but doesn't update the pointer. Fix: either forward
  the pointer alongside the visit, OR change the deepForce protocol so the target
  is always tenured (e.g., copy the list to staging before deepForce).
- [ ] **Walk Blackhole-state `tail[]` + `suspended.capturedWiths`** (round-2 N1).
  `clearBlackMarksOnException` reverts Blackhole → Suspended; OP_FORCE then
  re-reads tail. Treat Blackhole identically to Suspended in `walkThunk` and
  `Auditor::visitThunk`. SIGSEGV path on exception-heavy workloads under scavenge.
- [ ] **Mirror Auditor's walks to the Scavenger's** (round-1 #8). Either share
  a visitor pattern, OR codify the rule that each new scavenger root walk gets a
  matching auditor walk. The five missed mirrors today: bridge tables,
  primopReplacementMap, vBuiltins, importCache, callFlake, AttrSelectIC,
  `f.forceWriteTarget`. Without parity, `V3_DBG_NURSERY_AUDIT=1` gives false
  confidence.
- [ ] **Clear all `CFF_FORCE_WB*` flags + `forceWriteTarget = nullptr` in
  `clearBlackMarksOnException`** (round-2 N8). Only `CFF_FORCE_RETRY` is cleared
  today; the other writeback flags can survive across throws and trigger spurious
  `applyForceWriteback` on re-used VMStates.
- [ ] **Wire `Scavenger::walkedCUs` for transitive CU IC walking** (round-2 N3/N4).
  Field is declared at `gc.cc:114` with comment claiming transitive coverage but
  is never read or written. Populate from `walkClosure(c->cu)` and
  `walkThunk(t->suspended.cu)`; drain the set walking each CU's `attrSelectCache`.
  Closes the gap where bytecode-installed primop closures use `OP_ATTRS_SELECT_IC`
  against attrsets with nursery entries.
- [ ] **Track huge allocations and include in `Arena::blockRanges()`**
  (round-2 N11). The brute-scanner misses any tenured allocation > 4 MB (the
  `kHugeCutoff` path). Maintain a `vector<BlockRange> hugeBlocks;` and emit it
  alongside `blocks` from `blockRanges()`.
- [ ] **Zero `capturedWiths` in `recycleFakeClo`** (round-2 N9). Defensive
  cleanup; matches existing upvalue zero pattern. Currently safe-by-construction
  because every pool consumer immediately overwrites the field — but the
  invariant is fragile.

**TODOs — diagnostic-in-CI (highest leverage)**:
- [ ] **Wire `V3_DBG_NURSERY_BRUTE=1` into the lang+property+derivation-parity
  test runners**. The infrastructure already exists at `gc.cc:postScavengeBruteScan`
  and would mechanically catch every Round 1 missed-root class plus most Round 2
  issues. Today the gate is run manually on failing cases. Cost: ~2 days of test
  plumbing. Concretely:
  - Extend `run-nursery-tests.sh` p9-p12 to run with
    `NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 NIX_V3_NURSERY_SIZE=1 V3_DBG_NURSERY_AUDIT=1 V3_DBG_NURSERY_BRUTE=1`
    against the existing bench-nursery workloads plus a curated nixpkgs slice.
  - Parse stderr for `SCAVENGE AUDIT: ... reachable` and
    `SCAVENGE BRUTE: N tenured words point into nursery` (N > 0); fail the test
    on any hit.
  - Register as a meson `test()` so `ninja test` runs it.
  - Add a slower CI matrix entry that runs `--full` with these gates plus
    `NIX_V3_NURSERY_SIZE=1` (1 MB aggressive scavenge) across lang-tests +
    property-tests + derivation-parity. Non-default; nightly.

**Exit criteria**:
- All 8 sub-fixes landed and reverified via `V3_DBG_NURSERY_BRUTE=1` on the
  bench-nursery workload set: 0 hits across all p1-p12 cases.
- `run-nursery-tests.sh` p9-p12 in the default meson `test()` registry.
- The slower full-suite BRUTE pass exists as a nightly CI matrix entry.
- `RCA_VALUEPAIR_EVALUATED_2026-05-21.md` + `GC_AUDIT_ROUND_2_2026-05-21.md`
  updated with a closing "status" line per finding.

**Kill criterion**: if any sub-fix exceeds 2 days (against the <1-day estimate),
the codebase has hidden coupling we didn't see in audit. Stop, write up the
discovery as a follow-on RCA, and renegotiate the Phase 1.7 budget before
continuing.

**Rule 0 framing**: each sub-fix kills a specific missed-root hypothesis surfaced
in Round 2's audit. The aggregate kills the broader hypothesis "Phase C scavenge
is correctness-complete enough to default-on" — Round 2's BRUTE-based evidence
says NO; this phase says "now it is, and the diagnostic infrastructure proves it
on every CI run."

**Pre-Stage 3 not Stage 3**: explicitly noted because the temptation is to fold
these into Stage 3 itself. Don't — Stage 3 should land Phase D + Phase E on a
known-clean correctness baseline, not interleave correctness-and-perf work.

**Forbidden during Phase 1.6**: extending caps to per-thread / per-eval / per-import / daemon-mode granularity. Process-level only; deferrable. Don't gold-plate. No combined "memory-or-time-whichever-first" composite limit (users compose via setting both).

**Dependency**: depends on nothing. Can land in parallel with IR Phase F + H.

---

### Phase 2 kickoff conditions (added 2026-05-18 after Phase 1 closure)

Phase 1 closed early (3 days vs 10-day target). Phase 2 (cycle-handling decision) target was 2026-06-11 starting on Day 15. Given:

1. Phase 1's actual close came via the Option 4 hybrid bytecode wrapper — which **sidesteps** the underlying cycle/eager-force class for `derivation` / `derivationStrict` rather than resolving it. The eager-vs-lazy asymmetry (LESSONS §1.3) is still present in lower.cc; it just doesn't fire on the hot path anymore.
2. The new floor (`hello.drvPath` 30× slower) is NOT a cycle-handling problem. It's a per-op overhead + GC pressure + allocation-rate problem.
3. Phase 1.5 measurement spike now covers the drvPath force-rate decomposition.

**Phase 2 kickoff is conditional on Phase 1.5 returning verdict.** Three branches identified at planning time:

- **(a) Phase 1.5 says "cycle handling is highest leverage"** → run Phase 2 as currently scoped (THUNK_ALL vs CELL_EVERYWHERE decision; `pkgs ? lib` profile).
- **(b) Phase 1.5 says "GC pressure / allocation rate dominates the drvPath gap"** → pivot Phase 2 to "Phase 2′ — Stage 3 nursery default-on prep" (Phase D write barriers, scavenge frequency policy). Original Phase 2 work deferred.
- **(c) Phase 1.5 says "per-op dispatch dominates"** → pivot Phase 2 to "Phase 2″ — Phase 4 vm.cc decomp + computed-goto dispatch" prep. Original Phase 2 work deferred.

### Phase 2(R) — IR optimization passes A-H (CHOSEN BRANCH, in progress 2026-05-18)

Late on 2026-05-18 the team selected branch (b∧c hybrid): attack the per-op dispatch AND intermediate-allocation factors of the 200× force-rate gap simultaneously via IR-level optimization passes. The detailed plan lives in `IR_OPTIMIZATION_PLAN_2026-05-18.md` (8 phases A-H, each Rule-0-compliant with own hypothesis/exit/dependency/effort/risk).

**Justification for skipping formal Phase 1.5 on these factors**: the `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` decomposition serves as the measurement (factors identified: ~5-10× dispatch, ~2-5× intermediate allocations). Each IR phase has its own measurable validation. Phase 1.5 measurement is still relevant for Stages 10-13 (salsa / HAMT / JIT / multi-core) which the IR plan does NOT address.

**In-progress (2026-05-18 onwards)**:
- Housekeeping: re-baseline `bench/baselines/` + add hello.name / hello.drvPath / derivation-chain workloads to `workloads.toml`.
- Phase A: 1-shot beta reduction (capture-known lambdas), `opt_inline.cc` extension.
- Phase B: pure primop constant folding, extend `constantFold` for known-pure primops on literal args.
- Phase C: stream fusion (foldl'/map/filter chains → single loop).
- Phases D-H: capture-free lambda lift, selector recognition, static App spine fold, If fold, small-N genList unroll.

**Exit criteria (cumulative across A-H)**:
- `attrset-build-1k` regression closed.
- `fib33` from 1.6× → ≤1.4× TW.
- Per-op overhead drops from ~200× to ~20-30× **conditional on Stage 3 nursery closing the GC factor in parallel**. Without Stage 3, post-A-H residual is ~20-30× due to Boehm scan amortization.

**Kill criterion**: if Phase C (stream fusion) doesn't close `attrset-build-1k` to ≤30% over TW, the bytecode primop architecture has a structural issue beyond what IR fusion can fix; revisit.

**What this branch does NOT close**:
- The ~5-10× GC-scan factor — needs Stage 3 (nursery default-on with Phase D write barriers).
- The unknown caching gap — needs Phase 1.5 measurement of TW's caching, then Stage 10 (salsa) commit/falsify.
- Therefore: **even fully complete A-H + bench validation leaves the drvPath gap at ~20-30×, not the ≤3× target.** The team's stated ≤3× cumulative target assumes Stage 3 lands in parallel or shortly after.

Rule 0 framing: each A-H phase has its own falsification commit; the strategic choice to attack per-op + allocations together is implicitly justified by the decomposition. Phase 1.5 measurement is being run "in pieces" — decomposition for per-op/allocations (done in EXTEND_DERIVATION_INVESTIGATION); separate measurement needed for caching/HAMT/JIT/multi-core (still pending).

---

---

### Phase 1 — Close the A-series with iterative forceValue (Days 4-14, 2026-05-18 → 2026-05-28)

**Status: MET 2026-05-18 (3 days; closure commit `ecc99fd07`). See Appendix A for details. Section below retained as the original phase definition.**


A8 scaffolding is already partial. Finish it before continuing fakeClo / cycle work. Without iterative `forceValue`, deep stdenv hits C-stack overflow regardless of correctness.

- [x] **Audit every recursive `forceValue` call site** in vm.cc + primops.cc — DONE 2026-05-15, commit 988c92c0c. `ITERATIVE_FORCE_AUDIT_2026-05-18.md` enumerates 176 sites + 5 priority candidates (B1-B5).
- [x] **Convert (b) sites** to writeback-style iterative force — DONE:
  - Step 1 (commit 8df749725): callClosure primop-arg WHNF fast-path
  - Step 2 (commit 557d1fac8): primConcatLists / primConcatStringsSep WHNF fast-path
  - Step 3 falsification (commit 7a9ccc0e6): B3 valueEqual is NOT a meaningful target (depth probes 100-5000 all pass via TCO / shallow recursion)
  - Step 4 (commit e1dfd98c2): App-spine architectural conversion via identity-lambda specialisation.  Emit-time peephole + 3 runtime fast paths.  app-spine-10000 now passes (was failing at 5000); the hard-assertion cap was bumped accordingly.
- [x] **Remove the depth-2000 abort** if not already gone — VERIFIED 2026-05-15 (commit 8f3d80210 records the verification). Grep clean; `kMaxCallDepth = 5000` is the current ceiling, set by commit 377db9c16.
- [x] **Add a test that proves iterativeness** — DONE 2026-05-15, commit 13044c379. `v3-iterative-force-depth.sh` wired as meson test; hard assertions pass at 5000 for let-chain + curry, at 3000 for app-spine, with an informational probe at app-spine-5000 documenting the open architectural target.

**Phase 1 exit criterion**: `(import <nixpkgs> {}).hello.name` evaluates to a string under `NIX_V3_DIRECT_EVAL=1` *without C-stack overflow* (perf irrelevant — could be 100×). If it returns the wrong string or hits a different bug class, that's still progress: A7 closed, next bug visible.

**Kill criterion**: If after 10 days we still C-stack-overflow on hello.name, the iterative-conversion approach has missed a recursive site we cannot find. Pause and reconsider whether the recursion lives in C++ (forceValue) or in the bytecode dispatch (a misdesigned opcode chain).

**Forbidden during Phase 1**: New gates. New Phase 4 follow-ups. New fakeClo work. New lode/ docs except the audit doc above.

---

### Phase 2 — Cycle-handling architectural decision (Days 15-28, 2026-05-29 → 2026-06-11)

The current state is the worst possible: `partialBindingsRegistry` deleted; `Thunk::shapeCell` (its replacement) gated default-OFF; `NIX_V3_INHERIT_FROM_THUNK_ALL` (TW-blanket-laziness) carrying the load at 100× perf cost on `pkgs ? lib`. This is a decision, not an investigation.

**Decision path**:

1. **CPU-profile `pkgs ? lib`** under `NIX_V3_INHERIT_FROM_THUNK_ALL=1`. Linux `perf` or macOS Instruments. Identify the 100× source. (1-2 days)
2. **Based on the profile, choose one of two paths and commit to it for the remainder of the phase**:
   - **Path A — keep THUNK_ALL, fix the slowdown**: If the profile shows a single hot path (e.g. duplicated work in attrset construction), fix it. Cheaper if the bottleneck is local.
   - **Path B — finish `NIX_V3_CELL_EVERYWHERE`**: Make it default-on. Fix the lib.fix outer-`x` case (`Thunk::shapeCell` doesn't fire because no `OP_ATTRS_REC_INIT` in body — needs cross-thunk propagation per `CELL_UPDATE_EVERYWHERE_2026-05-12.md` Phase 3). Bigger but architecturally correct.

3. **Whichever path: delete the other one's gates** (`NIX_V3_NO_INHERIT_FROM_THUNK_ALL`, `NIX_V3_INHERIT_FROM_THUNK_FILTER`, etc., or `NIX_V3_NO_CELL_EVERYWHERE`).

**Phase 2 exit criterion**: `(import <nixpkgs> {}) ? lib` evaluates in ≤2× TW (i.e. ≤1 s vs TW's 0.5 s). Phase 1's hello.name still works. Either THUNK_ALL or CELL_EVERYWHERE is gone from the gate inventory.

**Kill criterion**: If neither path closes the 100× gap in 14 days, the assumption that this is a fixable optimization is wrong; the cycle-handling design itself needs revisiting. Convene a design review with one short doc; do not start STG-15.

---

### Phase 3 — Closure-pool reckoning (Days 29-35, 2026-06-12 → 2026-06-18)

A5/A6 sentinels are patch-on-patch. The recycle protocol is ill-defined. Two acceptable end-states:

- **A — Tag at allocation**: every closure gets an allocator-side tag that any consumer can verify; recycle is OK because the tag invariant holds. Touches every `closurePool.alloc` and `cur.closure =` site.
- **B — Retire the pool**: accept GC-tracked allocation; measure perf cost (likely small after Phase 1 + Phase 2 land). Simpler.

Decide based on Phase 2's perf signal. If v3-direct is already within 2× of TW, B is fine. If we're fighting for every percent, A.

**Phase 3 exit criterion**: A5-class corruption can no longer occur (proven by a stress test that hammers the alloc/recycle path with adversarial intermixed dispatches), AND fakeClo-specific sentinel bits (`_pad = 0xFA5E`, `CFF_FAKECLO_TAINTED`) are either justified by the new design or deleted.

---

### Phase 4 — Decomposition + hygiene consolidation (Days 36-49)

This is ongoing background work that becomes safe to start once dispatch-loop changes have settled.

- [ ] **Split vm.cc** into 5-6 files of ≤2000 LoC: `vm_dispatch.cc` (the opcode-body switch), `vm_force.cc` (forceValue + chase + iterative path), `vm_call.cc` (callClosure + run* family), `vm_bridge.cc` (TW interop, after Phase 2 has shrunk it), `vm_trace.cc` (NIX_TRACE_EVAL + V3_DBG_*), `vm.cc` left as the entry-point glue.
- [ ] **Embedded primops in opcode handlers** (`primHead`, `primTail`, `primLength`, `primElemAt` re-implemented inline near line 7580): either factor to shared inline functions or accept duplication with explicit "duplicated from primops.cc:NNN; keep in sync" anchors. No "Mirror primops.cc" prose comments.
- [ ] **Env-var consolidation**: per Phase 0 audit, fold the ≥30 retain-able diagnostic gates behind a single `NIX_V3_DEBUG` category bitmask (e.g. `NIX_V3_DEBUG=hot-force,blackhole,opcycle`). Keep ≤20 individual gates for things that genuinely need single-flag toggles.
- [ ] **Optimizer pipeline doc**: a single `OPTIMIZER_PIPELINE.md` describing the ordering, what each pass assumes about IR shape, and how to add a new pass. Supersedes the 7 individual file headers as the canonical source.

**Phase 4 exit criterion**: vm.cc ≤2 500 LoC. Env-var count ≤30. lode/ has ≤10 open docs (the rest renamed `*_RESOLVED.md` or `*_DEFERRED.md`).

---

## Part 4 — What success looks like, 8 weeks from now

By 2026-07-10, v3 should look like this:

- **Correctness**: `(import <nixpkgs> {}).hello.name` evaluates correctly under `NIX_V3_DIRECT_EVAL=1`. C1-C8 silent semantic gaps closed. Full nixpkgs `attrNames` completes.
- **Performance**: v3-direct within 1.3× of TW on `lib-evalModules-100`, within 1.2× on fib33. (Not parity; *measurable, monotonic progress*.) Bench harness re-run weekly.
- **Code health**: vm.cc split. Env-var count ≤30 (down from 169). lode/ has ≤10 open docs (down from 54).
- **Process**: No new RCA letter has been opened that wasn't closed within 7 days. Every gate added in the window has its retirement criterion in its own comment.

If we miss two or more of these by 2026-07-10, **the v3-direct-as-primary path itself needs reconsideration**, not just another phase letter. That is the meta-kill-criterion of this plan.

---

## Part 5 — Standing weekly cadence

- **Every Monday**: re-run `bench/` against last week's baseline. Commit the JSON. Diff perf cells; flag regressions > 5%.
- **Every Friday**: audit env-var inventory deltas. Net should be ≤0.
- **Every closed bug**: append a one-line RESOLVED row to its lode/ doc. If the bug was opened-and-closed in <7 days, no separate lode/ doc was needed and shouldn't have been written.
- **End of each phase**: explicit go/no-go decision against the exit criterion. Documented in this file's appendix.

---

## Appendix A — Phase decisions log (to be appended)

(Populate as phases complete. One line per phase: date, met/missed, follow-up.)

- Phase 0: **2026-05-15 MET** (1 day vs 3-day target). All
  exit-criterion bullets cleared:
  - CFF_TAINTED removed (commit 156939f43, -23 LoC, -1 enum bit)
  - v3_hook tombstones reaped (commit c6ef49599, 17 stale refs → 7
    historical-only)
  - analyseOccurrence wired into optimise() via opt-in
    NIX_V3_OCCUR_DCE + side-by-side NIX_V3_OCCUR_DCE_VALIDATE
    harness (commit f4f18cbbc)
  - DCE smoke tests positive + equivalence (commit 89526d7f3)
  - run-fail-tests.sh byte-exact diff with /pwd path sanitizer
    (commit 052cf4811) — 0/109 byte matches, 105 mismatch, 4 silent
  - V3_DBG_RETURN_SELF cached per lint pattern (commit 924ce6a00) —
    lint now CI-clean
  - C1-C8 fixtures committed (commit fd2c00af1) — 8 fixtures, 7 red
    + 1 regression-prevention
  - Bench baseline 2026-05-15-action-plan-baseline.json (commit
    ae095603e) — 23 workloads × 5 runs, ratios documented
  - ENV_VAR_INVENTORY_2026-05-15.md (commit c36ee9e07) — 167 gates
    categorized; KEEP=17, RETIRE-NOW=3, RETIRE-AFTER-X=147
  Follow-ups recorded: eager-bridge TLS deletion (post-Phase 0),
  Phase 4 prerequisite for NIX_V3_DEBUG=cat bitmask helper, Phase
  2 prerequisite for THUNK_ALL family collapse. Phase 1 starts now.
- Phase 1: **2026-05-18 MET** (3 days vs 10-day target).
  Exit criterion ("(import <nixpkgs> {}).hello.name evaluates to a string
  under NIX_V3_DIRECT_EVAL=1 without C-stack overflow"): CLEARED.
  Closure commit: `ecc99fd07` (Phase 1 exit MET document) backed by
  `7adc7e61f` (Option 4 hybrid landing) and `d3e41c13d` (Tag::App
  evaluated-result memoization).

  Results on hello.name and 7 sibling queries:
  ```
  TW:        hello.name → "hello-2.12.3"   0.47s
  v3-direct: hello.name → "hello-2.12.3"   0.58s   (+23%, within 1.4×)
  .pname / .version / .meta.description / .outputs / .system / .type
                                          all at parity
  ```

  Architectural wins landed in Phase 1 (16 commits over 3 days):
  - Path B localization (`ad95edfd2`) — root cause pinned to one
    nixpkgs site + one lower.cc gate (not patched directly; sidestepped
    by item below).
  - OP_CALL fun-force iterative (`7f5a392f4`, `830e3b66a`).
  - OP_ATTRS_SELECT_IC iterative + memoizing writeback (`a499fc2b0`).
  - Tag::App writeback for valueEqual/valueLess (`61f1ed96e`).
  - **Option 4 hybrid** (`7adc7e61f`) — bytecode wrapper for
    `derivation` / `derivationStrict`. Per-attr work runs as bytecode
    opcodes, moving the per-input C-frame growth onto the v3 frame
    stack where Phase 1.2's iterative-force handles arbitrary depth.
    This is the architectural fix, not the lower.cc gate identified
    in `ad95edfd2`.
  - T0-T12 bytecode-primop installation series (`5104a7270` →
    `537e06460`): foldl', map, filter, all, any, concatMap, partition,
    groupBy installed as v3 bytecode. T13-T17 reverted (non-callback
    primops stay as C; see LESSONS_LEARNED §4.11).
  - Tag::App `evaluated` field for memoization (`d3e41c13d`) — 16B
    per ValuePair; closed hello.name from 1.53s to 0.55s.

  **NEW open issue (Phase 1.5 / Phase 2+ territory)**: `hello.drvPath`
  and `hello.outPath` take 30-46s vs TW 1.3s (~30× slower). NOT a
  C-stack issue; characterized in
  `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md`. Force-rate
  ~5K/s vs TW's ~1M/s (200× per-op gap), decomposing into per-op
  dispatch (~10×) + GC scan overhead from Boehm 1GB+ arena (~5-10×)
  + extra intermediate allocations (~2-5×) + possible higher-level
  caching gap.

  Phase 1 exit criterion was specifically "no C-stack overflow,"
  which is met. The drvPath/outPath gap is Phase 2+ work mapped to
  Stage 3 (nursery), Stage 4 (strictness), Stages 5-6 (PICs).
  Memory: `project_force_rate_decomposition_2026-05-18.md`.
- Phase 2: [pending, target 2026-06-11]
- Phase 3: [pending, target 2026-06-18]
- Phase 4: [pending, target 2026-07-03]

---

## Appendix B — Source-of-truth references

- Honest cross-agent assessment, 2026-05-15 (this date's review session).
- `LESSONS_LEARNED_2026-05-15.md` — what worked / what didn't, distilled from git history.
- `ALIGNMENT_SCORECARD_2026-05-15.md` — vision-vs-reality scorecard (12 components + 6 drift items).
- `ROADMAP_TO_VISION_2026-05-15.md` — long-horizon Stages 1-8 (this plan = Stage 1).
- `CLEANUP_AUDIT_2026-05-09.md` — env-var and dead-code inventory.
- `CELL_UPDATE_EVERYWHERE_2026-05-12.md` — Phase 1.5 / 2 / 3 cycle-handling.
- `OPT_OCCUR_PLAN_2026-05-08.md` — occurrence-analysis pass (Phase B unwired).
- `RCA_FAMILY_DIVERGENCE_*_2026-05-11.md` (A1-A7) — fakeClo aliasing root-cause work.
- `REVIEW_2026-05-11/HONEST_ASSESSMENT.md`, `PROGRESS_OPEN_WORK.md` — Phase 3.3 cleanup status, open work.
- `V3_NATIVE_CONSTRAINT_2026-05-09.md` — V3-NATIVE constraint origin.
- `bench/baselines/2026-05-11-post-phase4.json` — current bench floor.
