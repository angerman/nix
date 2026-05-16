# Honest Assessment: v3 VM Recent Progress (2026-05-11)

Inputs to this synthesis:

- `PROGRESS_558_ARC.md` — the #558 cleanup commits and what they replaced.
- `PROGRESS_CORRECTNESS_STATE.md` — what test parity actually measures.
- `PROGRESS_PERF_TRAJECTORY.md` — bench trajectory and gaps.
- `PROGRESS_OPEN_WORK.md` — what's not done.
- `COMPREHENSIVE_REPORT.md` — this morning's semantic audit (TW vs v3).

I read all five and verified key claims against source. This document
is intentionally critical: where claims and code disagree, I report the
code.

---

## TL;DR

- **Real architectural progress today.** Phase 3.3a-g (seven commits)
  deleted ~600 LOC of the partial-Bindings registry — exactly the
  architecture this morning's audit identified as the root of the
  recurring `lib.fix` / `lib.extends` bug cluster. That removal is
  load-bearing and welcome.
- **The replacement isn't running.** `Thunk::shapeCell` (the
  STG-correct cell-update mechanism) is gated `NIX_V3_CELL_EVERYWHERE=1`
  in every read/write site. At default settings, neither the old
  registry nor the new cell mechanism is active. What's keeping v3
  alive is `NIX_V3_INHERIT_FROM_THUNK_ALL` (flipped default-on in
  Phase 3.2) blanket-thunkifying inherit-from like TW — a workaround
  that lets eager evaluation never reach the cycle.
- **The "142/142 lang tests" headline is real but narrow.** It measures
  only `eval-okay-*` stdout parity. The `eval-fail` runner uses a loose
  regex and never compares the golden `.err.exp` files. Every
  silent-correctness gap from this morning's audit (non-Bool boolean
  ops, missing-required formal, `+` numeric coercion, `//` operand
  order) reproduces on standalone `v3-eval` and is missed by every
  existing test.
- **v3 cannot run full nixpkgs in any mode that matters today.**
  v3-direct cycles (`STR_CONCAT: cannot coerce type to string` is
  today's specific error floor; the floor moves week to week — was
  `callPackage`, then `texlive`, then `libsForQt5`). v3-hook works on
  full nixpkgs since #547 default-on STG (April) but lib-evalModules
  is still 1.43-1.49× slower than TW.
- **Perf is competitive on synthetic compute, weak on the canonical
  use case.** fib33 = 0.66× TW, ackermann = 0.91× — the getenv-cache
  landing on 2026-05-09 was the inflection. lib.fix workloads are at
  parity once `nix eval` startup is amortized. The 25-28 ms vs 5 ms
  module-system gap is the real hole, and the canonical fix
  (hidden-class shapes / inline caches) has not been started.
- **Today's audit (C1-C7, S1-S11) is mostly still real at HEAD.** S2
  and S3 are stale only in the sense that the *specific functions*
  cited were deleted today; the architectural divergence remains,
  just relocated.

---

## What recent progress actually shipped

### #558 architectural cleanup (today, 11:12-16:45)

14 commits. Three substantive groups:

1. **Phase 1.5 — cell-update everywhere (opt-in).** `Thunk::shapeCell`
   (`closure.hh:131-156`) and wiring at five sites
   (`alloc.hh:360`, `vm.cc:3598/4585/4692/5367/7502`). Tests:
   `run-cell-update-protocol-tests.sh`,
   `run-thunk-all-regression-tests.sh`.
2. **Phase 3.2 — default-on flips** (`16d0adc81`):
   `NIX_V3_INHERIT_FROM_THUNK_ALL=1` and `NIX_V3_NO_PARTIAL_BINDINGS=1`.
   This is the *behavioural* fix — inherit-from is now blanket-lazy
   like TW.
3. **Phase 3.3a-g — deletion** (`0fde92c28` → `07a6352c1`).
   `partialBindingsRegistry`, `publishToNearestBlackThunkFrame`,
   `publishToAllThunkFrames`, `pickLargestLayer`,
   `lookupInPartialChain`, STG WHNF recovery in OP_FORCE,
   OP_ATTRS_SELECT registry-wide peek, withLookup chain peek,
   OP_RETURN consumers. ~600 LOC of "workaround for eager eval
   meeting cycles" gone.
4. **Phase 4 — perf path-compression** (most recent, today): thread-local
   `fakeClo` pool for Suspended-thunk forces, path compression for
   `Evaluated` thunk chains in `OP_FORCE` and `forceValue`, iterative
   App-spine walk.

### Recent perf wins

- **2026-05-09 getenv-cache** is the standout. `std::getenv` walks
  macOS env table linearly; caching 7 hot diagnostic env-vars + the
  per-`forceValue` `V3_DBG_FORCE_CALLSITE` saved 43-53% on fib's hot
  loop. This is what put v3 ahead of TW on synthetic compute.
- **Phase 2.3 inline WHNF check** before `forceValue` claims 12% CPU
  on nixpkgs THUNK_ALL (per Phase 2 commit messages — not yet in a
  bench JSON).
- **2026-05-10 Cheney nursery Phase A/C** landed but never made
  default-on; Phase D blocked on write-barrier design.

### Real correctness wins that survived

- WC-37 ghost-frame fix is fully resolved.
- WC-38 with-blackhole resolved via c95be6461 + 685262a6f.
- #547 default-on flipping fixed v3-fhook nixpkgs eval.
- #496 publishToNearestBlackThunkFrame restriction (fixed in
  7709cc20e). The publishing-everything class is gone *because the
  publishing-everything mechanism is gone*.

---

## What progress is more nuanced than it looks

### "142/142 lang tests pass" — true but unmeasured at the level that matters

- The runner pinned by `tests/functional/lang.sh` only diffs
  `eval-okay-*` stdout. v3 produces correct values for those — that
  is the real result and not nothing.
- `run-fail-tests.sh` reports 103/109 on `eval-fail-*` but uses a
  loose regex: "an error was raised" passes regardless of the
  message, position, or class. The golden `.err.exp` files are not
  compared.
- Three of this morning's verified C-series gaps were reproduced on
  standalone `v3-eval`:
  - C1: `null && true` returns `true` on v3; TW raises TypeError.
  - C2: `({a, b}: 42) {a = 1;}` returns `42` on v3; TW raises.
  - C5: `"foo" + 1` returns `"foo1"` on v3; TW raises.
  - C7: `(throw "lhs") // (throw "rhs")` throws `"lhs"` on v3;
    TW throws `"rhs"`.
- None of these has a `.exp` or `.err.exp` test that diffs the
  result. They're silently wrong in v3 and nobody knows.

### "Partial-bindings registry deleted" — true, but the replacement is off

- The deletion (Phase 3.3) is real and substantive.
- `Thunk::shapeCell` exists at `closure.hh:131-156` and is wired into
  the relevant opcodes — but every read and write is gated
  `if (s_cellEverywhere)`, defaulted off.
- The actual mechanism preventing cycles today is **eager =>
  thunkified at lowering**: `NIX_V3_INHERIT_FROM_THUNK_ALL=1` makes
  v3 match TW's blanket-laziness on inherit-from, so the cycle
  never forms. That is a correct mechanism; it's just not the
  STG-correct cell-update story the planning docs describe.
- The advertised `NIX_V3_KEEP_PARTIAL_BINDINGS=1` escape hatch from
  commit `16d0adc81` is **not wired** — `grep` returns zero source
  matches. There is no opt-back path if a workload regresses.

### "v3 faster than TW" — true on synthetic, untrue where it matters

- fib33 = 0.66× TW, fib30 = 0.71×, ackermann = 0.91×. These are
  real and reproducible.
- Real-world `nix eval`-style lib workloads are at parity (1.02-1.13×
  wall, but most of that is the 50ms `nix eval` startup floor; net
  eval delta is 1-3 ms over TW's ~3 ms).
- **lib-evalModules-100 is still 1.43-1.49× slower** (25-28 ms v3 vs
  ~5 ms TW). The module system is the canonical NixOS use case and
  has not budged.
- **Full nixpkgs**: doesn't complete in v3-direct
  (`OP_WITH_LOOKUP cycle on 'callPackage'` at last
  measurement); v3-hook completes since #547 but cardano-node
  hasn't been re-measured since 2026-05-04.
- **Phase 3 / Phase 4 have not been re-baselined.** The bench JSON
  at `bench/baselines/2026-05-09-with-modules.json` is two days
  stale; today's deletions and pool work are claimed-better but
  unmeasured.

---

## What's actually broken or open

### Cannot run nixpkgs end-to-end

- **v3-direct + full nixpkgs**: cycles. Error floor moves week to
  week. Today: `STR_CONCAT: cannot coerce type to string`. Prior:
  `callPackage`, `texlive`, `libsForQt5`, qt5
  `OP_ATTRS_SELECT`. Each fix exposes the next.
- **v3-direct + lib.fix toFix**: cycles. `Thunk::shapeCell`
  structurally doesn't fire for the outer `x`-thunk in `lib.fix
  toFix` (per `CELL_UPDATE_EVERYWHERE_2026-05-12.md`).
- **Full nixpkgs under THUNK_ALL**: times out >120s where TW takes
  0.7s. Phase 4 is the active attempt to bring this completable but
  qualitative-only at HEAD.
- **v3-standalone derivation work**: fake-store fallbacks at
  `primops.cc:6241-6248` and `:4900-4904` produce
  `/v3-fake-store/<fnv>-<name>(.drv)`. Useful for tests, not for
  building.

### Audit findings still real

This morning's `COMPREHENSIVE_REPORT.md` listed 7 critical (C1-C7) and
11 significant (S1-S11) divergences. Cross-referencing against today's
HEAD:

- **C1-C7 (silent correctness gaps)**: all still real. The #558
  cleanup did not touch them. Each is a small mechanical fix
  (~3-4 days total mechanical work).
- **S1 (`Tag::Blackhole` as value)**: still real. The propagable
  blackhole-value singleton still exists.
- **S2/S3 (STG WHNF / publish-everywhere)**: the *specific
  functions* cited in the audit (`publishToNearestBlackThunkFrame`,
  `lookupInPartialChain`, `pickLargestLayer`) are deleted as of
  today. The architectural divergence remains — it's just located
  in `Thunk::shapeCell` now, behind the opt-in gate.
- **S4 (with-lookup swallows BlackholeError)**: appears unchanged
  at `vm.cc:759-808`.
- **S5 (inherit-from heuristic)**: effectively addressed by Phase
  3.2 flipping THUNK_ALL default-on — though that's "match TW by
  being lazy", not the STG-correct cell-update story.
- **S6-S11**: unchanged.

### Bridge layer is bloating

- `v3_hook.cc`: 4147 LOC.
- 30+ `NIX_V3_NO_*` env-var safety nets.
- 7 separate bridge-depth gates.
- Bridge surfaces disagree with each other on exception
  re-throw (this morning's C3/C4 finding).
- The narrative says "bridge is a thin FFI shim". The code says
  "bridge is a permanent workaround surface."

### Observability is inert under v3

- No `EvalProfiler` hooks invoked.
- `NIX_COUNT_CALLS` gets zero data.
- `--trace-function-calls` does nothing.
- `nrFunctionCalls`, `functionCalls[lambda]`, `primOpCalls[name]`,
  `primOpTimerStack` all uncounted.
- `trylevel` not tracked.

### Stale narrative

- `bytecode.hh:225-274` docstrings still describe the deleted
  publish semantics.
- `CELL_UPDATE_EVERYWHERE_2026-05-12.md` carries a *future* date
  (today is 05-11) — date drift in the planning corpus.
- `CFF_TAINTED` is still *read* at `vm.cc:3573` but has zero
  writers — dead code from the deleted registry era.
- `NIX_V3_NO_STG_WHNF` lives on in a comment only.
- One real `TODO` marker in the codebase: `lower.cc:1778`,
  "root-cause and..." note on the self-dot heuristic, open since
  2026-05-07.

---

## Net assessment

**v3 is a serious, technically credible bytecode VM that has made real
architectural progress in the last week**, particularly today. The
#558 cleanup removed a known-bad mechanism that this morning's audit
independently identified as the source of an entire bug family. That
deletion took courage — the easier path would have been to keep
patching the workaround.

**v3 is also not close to shippable as a TW replacement for the
canonical NixOS / nixpkgs workload.** The replacement architecture
(cell-update everywhere) is staged but opt-in; full nixpkgs eval
doesn't complete in any mode without the THUNK_ALL workaround, and
even with it, lib-evalModules is 1.5× slower than TW. v3-direct still
hits a moving error floor. The bridge layer is growing, not shrinking.
Test coverage measures parity at a granularity that misses every
silent-correctness gap audited this morning.

**What would shipping require?**

1. Close C1-C7 silent gaps (~3-4 days mechanical).
2. Wire profiler + max-call-depth + observability (~1 day).
3. Unify bridge re-throw policy (~3-5 days).
4. Either finish cell-update-everywhere (no estimate, has hit
   conceptual walls per `CELL_UPDATE_EVERYWHERE_2026-05-12.md`) or
   formally accept THUNK_ALL as the lazy strategy and delete the
   `s_cellEverywhere` gate.
5. Make lang `eval-fail` tests actually diff `.err.exp` so the
   silent-correctness gaps stop hiding.
6. Add tests for C1-C7 specifically.
7. Get full nixpkgs eval reliably completing under v3-direct (no
   estimate).
8. Close the module-system 1.5× gap (hidden-class shapes — not
   started).

Items 1-6 are weeks of mechanical work. Item 7 is the real risk: each
fix exposes the next. Item 8 is the largest unstarted piece of the
roadmap.

The honest summary is: **the architecture is moving in the right
direction this week, but the project is mid-flight and the rhetoric
is ahead of the code.** Both the deletion and the un-wired
replacement are evidence of that.
