# v3 Lessons Learned — 2026-05-15

Distilled from ~1 380 commits since 2026-03-01, plus the older lode/ archive and project memory. This is the canonical "what works / what doesn't" reference. Re-read before opening any new RCA or before adding any architectural mechanism.

Companion docs: `ACTION_PLAN_2026-05-15.md` (immediate phases), `ALIGNMENT_SCORECARD_2026-05-15.md` (vision vs reality), `ROADMAP_TO_VISION_2026-05-15.md` (long-horizon stages).

---

## Part 0 — The main issue (multi-agent synthesis, 2026-05-15)

Three independent multi-agent reviews on "what kept v3 in circles" converged. They identified three layers of one underlying epistemic failure.

**The failure was epistemic, not technical**: models were never killed, only gated. As long as N competing models coexisted under N env-var flags, no model could be wrong, so none needed fixing.

The three layers:

1. **Strategic (upstream)**: The hybrid TW-interop premise (`NIX_USE_V3=1` cutover hook in libnixexpr, 2026-04-27) was the architecture-of-record for ~7 months past its internal refutation. The retraction memo `cf12c1880` is *literally titled* "retract TW-routing suggestion (architectural mistake)" and dates 2026-05-09. `v3_hook.cc` was deleted on 2026-05-13 (4 154 LoC, terminal). The diagnostic that would have surfaced the mistake earlier (`NIX_TRACE_EVAL`) landed 2026-05-14 — one week *after* the retraction. Every bridge-boundary cascade (A-series, STG-series, #498-#516-#546-#548-#558) is downstream of this premise.

2. **Architectural (the specific incompatibility)**: v3's Value model lacks TW's per-binding update cell (`nix::Value*`). v3 substituted "stable Thunk pointer," but Thunks are per-call, not per-binding. Every parallel mechanism in v3 (`Tag::Slot`, `partialBindingsRegistry`, `Thunk::shapeCell`, `publishToNearestBlackThunkFrame`, fakeClo pool, cross-thunk propagation) is a workaround for that missing primitive. Under the hybrid premise (layer 1), this incompatibility surfaced as a new cascade at every bridge boundary; in a pure-v3 design it would surface as plain v3 bugs that change-v3-to-fix.

3. **Methodological (the process that let it persist)**: every investigation exited via an opt-in gate (`NIX_V3_NO_X`, `V3_DBG_Y`) instead of a falsification commit. The gate IS the un-falsified hypothesis. STG-1 through STG-14b never killed STG-1 — each letter added a flag. A1-A7 contains an explicit "REFUTED" in A4 that did not stop the series, just renamed the hypothesis. Even #558 Phase 3.3 — the largest cleanup of the project — left two competing models (`Thunk::shapeCell` and `THUNK_ALL`) running side-by-side under different gates. "Ship-it-gated-opt-in" is the deferral instrument that prevents commitment.

**Single sentence**: the project carried a hybrid TW-interop premise for ~7 months past its internal refutation, because investigations exited via opt-in gates instead of falsification commits, so the premise never had a "killed" state.

**Corollary observation**: immediately after the first commit-as-killed-model on 2026-05-09 (`cf12c1880` "architectural mistake" memo), real architectural progress happened in days, not weeks — v3_hook.cc deleted (`e8d7c3885`, 4 154 LoC out), partialBindingsRegistry retired (`07a6352c1` + 6 siblings, ~1 000 LoC out), NIX_TRACE_EVAL infrastructure (`d17afe9e1` + 2 siblings). Killing a model unblocked the codebase.

### The corrective rule

> **Every commit body must answer: "what hypothesis does this kill?" If it kills none, it doesn't merge.**

A commit may kill a hypothesis by:
- Falsifying it (negative measurement, failing repro, etc.) — the commit removes the old code AND the env-var that controlled it.
- Confirming it and committing default-on — the commit removes the alternative AND its gate.
- Renaming the issue to a fresh top-level investigation with a new budget — the old letter/issue must be marked CLOSED-RENAMED, not amended.

A commit may NOT exit an investigation by:
- Adding an opt-in gate so "both can coexist for now."
- Adding a `V3_DBG_*` diagnostic to "narrow it next time" without a kill criterion for the next time.
- Reverting + reapplying without a measurement between.
- Producing a `*_findings.md` document and leaving the code unchanged.

This rule subsumes the more specific tactical rules in §1.4 (bench-before-merging), §1.5 (cascade anti-pattern), §3.7 (revert-reapply churn), §3.8 (gate proliferation), §3.9 (multi-letter cascades). Those are surface manifestations; Part 0 is the underlying rule.

### How to recognize the failure mode

If you find yourself:
- Naming the next RCA letter or STG number, **STOP**. Either the previous letter is still open (close it first) or this is a new top-level issue (give it a number, not a letter).
- Adding `NIX_V3_*` or `V3_DBG_*`, **STOP**. State the hypothesis. State what would falsify it. State when the gate retires.
- Writing the next lode/ `*_2026-XX-XX.md` while the previous one has no RESOLVED row, **STOP**. Close the previous one or rename it `*_DEFERRED.md` with a one-line reason.
- Reverting a recent commit, **STOP**. Don't reapply without a measurement that justifies one side or the other.

---

## Part 1 — Established constraints (load-bearing rules)

These are not opinions. They are conclusions reached the hard way.

### 1.1 V3-NATIVE constraint

**Rule**: V3 owns evaluation. TW (libexpr) is permitted *only* at FFI leaves (store, paths, derivations, file I/O, eval-state parse).

**Origin**: Commit `cf12c1880` ("v3 #547 Phase 3 memo: retract TW-routing suggestion (architectural mistake)") + commit `60c6a7666` ("V3-NATIVE constraint memo"). Confirmed in memory (`feedback_v3_native_constraint.md`).

**What's forbidden**:
- Routing v3 thunks through TW.
- Routing v3 cycles through TW for recovery.
- Bridging mid-eval `Value*` between v3-managed and Boehm-managed memory as a workaround for a v3 bug.
- "Use TW as a safety net for v3" — every time this has been tried, it has eventually been retracted.

**Why**: every cross-boundary trip costs marshalling (copy or wrap), violates GC ownership (v3's nursery vs Boehm), and erases the architectural reason v3 exists.

### 1.2 V3-native primops are CORRECT (not drift)

**Rule**: Pure-data primops (list, attrset, string, arithmetic, comparison) stay v3-native. They are not replaceable by FFI calls to cppnix's primops.

**Origin**: Practical experience marshalling v3 Values ↔ TW Values at primop call boundaries (commits `857227578`, `951bdef57` revert v3→TW fast paths that turned out to be regressions).

**Why**:
- v3 Values live in v3-nursery / v3 allocator; TW Values live in Boehm-managed memory.
- Marshalling at every primop call is a per-call copy or wrap — for `map`, `filter`, `foldl'`, that's millions of trips on real workloads.
- Crossing the boundary is GC-unsafe: scavenge cannot move v3 Values held inside a TW primop; Boehm collection cannot safely move TW Values held inside a v3 primop. Either marshal-by-copy (slow) or pin (correctness hazard).
- The 8 336 LoC of primops.cc is partly correct architecture, partly genuine duplication (small fraction). Audit and classify; don't blanket-shrink.

**What goes to FFI**:
- Store operations (deriving, `addToStore`, paths, IFD).
- File I/O (`readFile`, `readDir`, `findFile`).
- Eval-state operations needing cppnix infrastructure (`builtins.fromJSON`, `import` at runtime).
- Path normalization, store-path validation.
- Symbol/Name interning shared with cppnix.

### 1.3 Eager-vs-lazy asymmetry is the dominant bug class

**Rule**: When a "v3-direct fails on nixpkgs" bug surfaces, suspect `lower.cc` emit-time decisions about thunkification before suspecting runtime/forceValue/cycle/blackhole logic.

**Origin**: `#496` (publishToNearestBlackThunkFrame), `#497` (ExprOpUpdate inherit-from thunkify), `#498` (always-thunkify regression), `#516` (callPackage with-scope), `#546` (OP_ATTRS_REC_INIT split), `#548` (inherit-from lazy), `#577` (hello.name cycle), `#583` (matchAttrs memoization) all collapse to: lower.cc thunkifies differently than TW's blanket `maybeThunk`. Memory note `feedback_v3_native_constraint.md` says: "When similar bugs surface, look at lower.cc emit-time eager-vs-lazy asymmetries, not runtime chain."

**Implication**: The architectural fix is uniform-lower-everything-as-thunk + strictness analysis (Roadmap Stage 4). Patching individual symptoms is correct *only* as a stop-gap; do not declare a fix landed if the asymmetry pattern is still present.

### 1.4 Bench before merging perf claims

**Rule**: Every commit that claims a perf improvement must have a measured before/after number in the commit body. "Theoretically faster" / "should help" claims must include a one-line reason why measurement was skipped.

**Origin**: Phase 4 fakeClo pool + path compression (commits `34a59442e`, `ba2476e73`, `dfde0e33f`, `b4b2c2a72`, `93711cdf2`) shipped 5 follow-up commits collectively claimed to optimize Suspended-thunk forcing. The post-Phase-4 bench (`72ad0c9f9`) reported **ZERO cells flagged at ±5% threshold** — the entire phase was a no-op on benched workloads. Burned 1 week of effort, kept the sentinel-bit corruption (A5/A6) it introduced.

**Counter-examples (real wins, measured)**:
- `#455` per-lambda OD blacklist: **38 s → 1.35 s on hello.name** (commit `1e040415f`).
- getenv-cache for `V3_DBG_FORCE_CALLSITE` in forceValue: **43-53% on fib hot loop** (~5 lines of code, `static const bool` cache).
- `#542` emit-time defer around OnceLinear bindings: **-30% bytecode on fib**.
- A8 iterative writeback force: removes the depth-2000 hard-abort; unlocks deeper eval.

The pattern: real wins are simple, measured, and provable. Speculative architectural reshuffling rarely pays.

### 1.6 v3 uses Boehm conservative GC today; Cheney nursery is designed but OFF

Despite the Cheney nursery design in `CHENEY_NURSERY_DESIGN.md` and the substantial discussion of "the nursery" in lode/ docs, **the operative allocator in v3 today is Boehm-Demers-Weiser conservative GC**, inherited from cppnix. Concretely:

- Phase A (allocator) and Phase C (scavenge fast paths) of the nursery LANDED but are gated `NIX_V3_NURSERY=1` opt-in (default-OFF).
- Phase D (write barriers) is unresolved per `78835a047` ("write-barrier insufficient alone").
- Phase E (scavenge frequency policy) hasn't been scoped.
- The closure-pool / fakeClo recycling pool sits ON TOP OF Boehm — it's a fast-path for one specific shape (Suspended-thunk frame closures), but underlying memory is Boehm-allocated.

Operational consequences observed on real workloads (e.g. `hello.drvPath` per `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md`):

- **Heap grows; doesn't shrink.** The 1GB Boehm arena watermark during long evals stays at the high-water mark even when 95% becomes free internally. Future allocations land in free regions; resident set stays high.
- **Conservative scanning retains defensively.** Boehm sees `uintptr_t`-shaped words; anything that looks pointer-like keeps that heap region alive. Tagged Values (16-byte, tag+payload) sometimes have pointer-looking payloads, sometimes don't — Boehm errs toward keeping alive.
- **No generational separation.** Short-lived intermediates (Tag::App entries, intermediate Bindings from bytecode primops, A-normal-form thunk-bindings) pay full mark-sweep cost. A nursery's whole value proposition (most objects die young; reclaim in O(survivors)) is unavailable.
- **Scan cost grows with arena size.** Each GC trigger walks the full live region; with arena past 1 GB, each scan amortizes into every force.

**Implication for the 200× force-rate gap on hello.drvPath**: the Boehm-scan-amortization factor contributes an estimated ~5-10× of the observed gap. It is NOT the whole story (other factors: dispatch ~10×, intermediate allocations ~2-5×, possible caching gap unknown), but it IS load-bearing. Closing this factor requires Stage 3 (nursery default-on with Phase D write barriers landing), not just optimization elsewhere.

**Implication for new code**: when designing primops or IR transformations, be aware that allocation in v3 is currently cheap-to-emit but expensive-amortized via Boehm scans. Reducing allocation rate at the source (Stage 4 strictness analysis, fusion, etc.) helps almost as much as the nursery would, because Boehm scans less when there's less to scan. The two are complementary, not substitutive.

Reference: `feedback_v3_nursery_cstack_safety.md` memory (existing); `project_force_rate_decomposition_2026-05-18.md` memory (added 2026-05-18).

### 1.5 Phase-letter cascades signal investigation-without-convergence

**Rule**: If a workload has an open RCA letter, the next divergence on it is a sub-letter on the same root-cause track. STG-15 may not exist until STG-14b is closed or explicitly retired.

**Origin**: `{family}` A1 → A2 → A3 → A4 (with REFUTED!) → A4-take-2 → A5 (real fix) → A7 (workaround) → A8 (scaffolding) → A9 → A12, all on hello.name + nixpkgs eval. STG-1 through STG-14b on `#498`/`#516`/`#558`. Each cascade burned 1-2 weeks; the underlying architecture didn't move.

**Counter-pattern**: architectural commitment. If letters A1-A3 reveal a class of bug, commit to the architectural fix in A4. Do not keep RCA'ing the same workload from a fresh hypothesis each Monday.

### 1.7 Make it work right before making it fast

**Rule**: Correctness/parity work before performance work. Optimizing a system whose semantics are still moving produces fast-but-broken. Stage architectural perf investments (nursery, write barriers, hidden classes, PICs) only after the correctness foundation is stable.

**Origin (the cautionary tale)**: Codified 2026-05-20 after a 5-day stretch where the author of these docs repeatedly framed "Stage 3 nursery is load-bearing for perf; the team keeps deferring it" as a structural concern. The team was correctly prioritizing correctness/parity bug fixes (#665 `nix-eval-impure` cascade; #666 derivation equality; #667 OP_ASSERT operand force; #668 Tag::Uninitialized in STR_CONCAT; #669 rich `nix eval` printer parity; **#670/#671 dangling string_view across symbol intern — root-cause UAF fix**; #672-#675 string-context preservation across `toJSON`/`baseNameOf`/`dirOf`/`toXML`). Each fix changed the GC reachability shape. Stage 3 nursery work done BEFORE these landed would have needed revisiting after every parity fix — partial rework of write-barrier sites, of cell-tracking, of FFI-bridge promotion points.

**Why**:

1. **Optimization needs a stable semantic target.** Each parity fix changes what's reachable, what's a context-bearing Value, what's equal, what's a root. Phase D write barriers built against today's semantics get invalidated by tomorrow's parity fix.

2. **Bugs reveal architectural truths.** The cluster of string-context bugs (#672-#675) reveals what kinds of Values cross primop boundaries. The nursery design has to accommodate that. Building the nursery before the bugs are fixed means building against an incomplete model.

3. **Pillar 2 (cardano-node) FFI bridge work is correctness, not perf.** Commits `c11bb7374` (shallow-bridge for nAttrs + nList) + the derivationStrict-scoping work look like "Pillar 2 prep" but are actually getting cross-VM-state semantics right. Prerequisite for *any* perf work crossing the boundary.

4. **The v3-hook deprecation is the original case study of this rule.** We spent 7 months on the hybrid architecture (perf wasn't the issue — correctness was). Stage 3 deferral today applies the lesson: don't optimize a system whose semantics aren't pinned down. See LESSONS Part 0 + `cf12c1880` ("architectural mistake" memo).

**How to apply**: when tempted to advocate for an architectural perf investment, first verify the correctness foundation:

| Signal | Threshold for "correctness stable" |
|---|---|
| Parity-bug discovery rate | ≤ 1/week sustained |
| C1-C8 silent-semantic-gap fixtures | all 8 green |
| `nix eval --json` byte-exact match with TW | holds for 1 week on representative corpus |
| cutover-parity test | 142/142 holding 5+ consecutive days, no regressions |
| Property tests | 580/580 under randomized inputs holding |

If <4 of these hold, perf investments are premature. Refocus on the missing signal first.

**Anti-pattern**: chanting "Stage X is load-bearing for perf, why isn't anyone working on it?" without checking the correctness signals. Calling it "architectural debt deferred" when it's actually "architectural debt correctly deferred until foundation is stable." This is what I (Claude in past turns) did for 5 consecutive days; codifying so future sessions don't repeat.

**Counter-counter-pattern**: don't use this rule to defer perf work *indefinitely*. The watch is "correctness signals converging," not "correctness signals perfect." When 4-of-5 hold for 5 days, perf work becomes the next priority. Don't let bug-fix work fill all available time.

**Concrete corollary on observed velocity**: correctness-first doesn't materially delay the endpoint. Perf work on stable semantics is *faster* and doesn't get reworked. Yesterday's projection that pushing Stage 3 nursery to ~2026-06-03 (post-correctness-convergence) only slips full-ROADMAP-end-state by ~1-2 weeks vs starting it 2026-05-19 — because skipping rework saves roughly the same time the deferral spends.

**Reference**: this rule is the engineering variant of "make it work, make it right, make it fast" (Kent Beck) — but more conservative: make it work *right* before making it fast, because the alternative is fast-but-broken.

---

## Part 2 — What worked (patterns to repeat)

### 2.1 Demolition over patching

When a mechanism is not paying for itself, **delete it** rather than maintain it:
- `e8d7c3885` — v3_hook.cc deleted (4 154 LoC). Terminal. The v3-hook mode is genuinely gone.
- `07a6352c1`, `5a67f4e6f`, `4d42bc200`, `db74e8544`, `0fde92c28` — Phase 3.3a-g deleted `partialBindingsRegistry` + helpers (~1 000 LoC).
- `45b837250` — review-2026-05-05 §5 removed 5 dead opcodes.

Both demolitions were correct. The cost of maintenance was higher than the cost of the replacement (or no replacement). The pattern: when a mechanism has accumulated 5+ workarounds (gates, sentinels, escape hatches) for its own corner cases, that mechanism is a candidate for deletion.

### 2.2 Inline WHNF check before forceValue

`#558` Phase 2.3 parts 1-4 (`bebb0d5ae`, `7e476f42a`, `b2fd95988`, `9c17005fb`): check thunk-Evaluated tag *before* calling `forceValue` in hot primops (`valueEqual`, `filter`, `all`, `any`, `partition`, `concatMap`). Real fast-path wins. Mechanically simple. Generalizes.

**Lesson**: every primop that takes a `Value*` should fast-path the Evaluated case inline. Pay the dispatch only on Suspended.

### 2.3 Typed exceptions over string matching

`#440` / `cf253b455` — typed `BlackholeError` class replaces `strstr` matching on what amounted to error-message-parsing. Correctness + clean.

**Lesson**: never recover error semantics from formatted strings. If you need to act on an error class, that class needs a type.

### 2.4 In-memory content-keyed cache

`#495` follow-on (`0a480cfb0`): in-memory cache keyed by content hash. Real bench delta.

**Lesson**: when the same thunk-equivalent body recurs across many positions, content-keying lets one cell serve all of them. This was a real Nix-specific insight — nixpkgs has massive duplication at the IR level.

### 2.5 Iterative writeback for forceValue

A8 series (`f6bf3fe8d`, `f82a2f725`, `5d9909d8c`, `f5804ea05`, `3d997fc7d`): replace C-recursion through `forceValue` with frame-level retries on ~14 opcodes. Lets `377db9c16` remove the depth-2000 hard-abort.

**Lesson**: any architecturally-deep evaluation operation (force, dispatch, scavenge) must be iterative, not C-recursive. C-stack overflow is a structural failure mode, not a tuning problem.

### 2.6 Cross-evaluator differential trace

`NIX_TRACE_EVAL` (`d17afe9e1`, `f8539922a`, `975b711f4`): byte-exact force-event trace. Proved that TW and v3-direct match on the first 353 632 events of hello.name. Reframed A9 from "eval-order divergence" to "v3 doesn't stop when answer is in hand."

**Lesson**: when symptom doesn't match hypothesis, build the diagnostic that produces ground truth. One day spent building NIX_TRACE_EVAL saved weeks of A-series hypothesizing.

---

## Part 3 — What didn't work (patterns to avoid)

### 3.1 TW-routing as cycle/blackhole recovery

Tried; explicitly retracted (`cf12c1880`). See §1.1.

### 3.2 v3→TW scalar fast path

`951bdef57` — "step B: revert v3->TW scalar fast path (measurable regression)." The hope was that bypassing v3 dispatch for simple-typed values would help. It didn't, because the bypass cost more than the dispatch.

**Lesson**: TW-bridging is never a perf win at fine granularity. The boundary is too expensive.

### 3.3 Phase 5 polarity flip

`1edb2e982` — "v3 #427: revert Phase 5 polarity flip — cardano-node regression."

**Lesson**: any "flip a default" perf experiment must be benched on a real workload (cardano-node, libsForQt5, full nixpkgs) before merging, not just on micros. cardano-node has caught at least 3 unmerged regressions.

### 3.4 Direct-AttrSelect formals

`857227578` — "v3 EVAL-COMP §3.3 REVERT: direct-AttrSelect formals broke laziness." A specific optimization (skip the closure for formals-shaped attrsets accessed directly) violated laziness rules.

**Lesson**: laziness invariants apply to every shape that v3 emits. Specializing a "shape" without a strictness analysis behind it almost always breaks something.

### 3.5 On-demand call-hook precompile

`2b9295437` + `cfd6a7ef1` — attempted, reverted with postmortem. The work was real but produced no behavioral change; the precompile-coverage upgrade postmortem was the only honest output.

**Lesson**: when a "performance" experiment produces no behavioral change *and* no perf delta, it was the wrong experiment. Write the postmortem, do not re-roll-out.

### 3.6 Phase 4 fakeClo pool + path compression

5 commits, ~1 week effort. Bench delta: zero cells moved ±5% (`72ad0c9f9`). Introduced the A5 sentinel-corruption bug class.

**Lesson**: see §1.4. Bench before declaring victory. Also: any allocation pool with a recycling protocol needs sentinel-free design from day one — A5/A6 chased sentinel bugs that wouldn't exist with a generational GC.

### 3.7 Revert-and-reapply churn

`4a616bbe5` + `666998bd7` — Revert + Reapply of "partial-Bindings peek to OP_ATTRS_SELECT". Then `121ce7835` deleted the reapplied code three days later in Phase 3.3.
`541d4390a` + `224f912e2` — Revert + Reapply of nursery leaf-tag fast paths.

**Lesson**: if you're considering reverting a recent commit, the original commit lacked a measurement. Don't reapply without one. The reapply pattern signals indecision.

### 3.8 Gate proliferation

169 unique env vars accumulated, including `getenv("X")` and `getenv("Y")`. Most are bisect tools from individual investigations that never got reaped.

**Lesson**: every gate has an inline retirement criterion. PRs that violate are reverted, not amended. `test/lint-no-inline-getenv.sh` exists for a reason.

### 3.9 Multi-letter RCA cascades

See §1.5. The pattern: A1-A7 fakeClo, STG-1 through STG-14b inherit-from, the entire #558 Phase 1, 1.5, 1.5b, 2, 2.2, 2.3-parts-1-4, 2.4, 3.1, 3.2, 3.3a-g, 4, 4-follow-up-x4 ladder.

**Lesson**: budget 1 week per investigation. If letters A1-A4 don't converge on an architectural fix by day 7, the hypothesis is wrong; stop generating diagnostics, write the architecture doc.

---

## Part 4 — Specific Nix-domain knowledge

Things that are true about Nix workloads, not generally about VMs:

### 4.1 nixpkgs has massive IR-level duplication

The same `{ pkgs, lib, ... }: ...` callPackage closure shows up 5 000+ times. Content-keyed cache (`0a480cfb0`) exploits this. So does any future selector-thunk / sharing optimization.

### 4.2 callPackage is the canonical stressor

callPackage uses `with-scope` semantics that are particularly fragile to eager/lazy asymmetries. Bugs surface here first: `#516`, `#548`, the entire #498 cascade. Tests: `known-fail-callpackage-with.sh`.

### 4.3 `lib.fix` is the second stressor

The `x: f x` pattern interacts with attribute-set shape in subtle ways. The outer `x` thunk has no `OP_ATTRS_REC_INIT` body, so `Thunk::shapeCell` cell-update never fires. Memory: `libsForQt5_deferred.md`.

### 4.4 Module system has the canonical perf cell

`lib-evalModules-100` is the canonical 1.5× gap measurement. If a perf change doesn't move this cell, it doesn't move real work.

### 4.5 darwin/default.nix:956 is the hello.name cross-ref

`libllvm.buildCommand ↔ binutils-unwrapped.buildCommand` cross-ref via `getOutput`. v3 eval-order divergence surfaces here on hello.name. Memory: `project_577_hello_name_cycle.md`. NIX_TRACE_EVAL showed it's actually a post-result over-eval, not mid-eval divergence (memory: `project_578_eval_trace.md`).

### 4.6 cardano-node catches what micros miss

cardano-node has the broadest attribute graph that ships with a v3 bench harness. It catches what fib33/ackermann don't: shape diversity, deep with-scopes, real-world cross-refs. Every default-flip must be cardano-node-benched.

### 4.7 `builtins.isAttrs` vs `builtins.hasAttr` diverge wildly

A12 found: `builtins.isAttrs (import <nixpkgs>{})` runs 11.5M forces in v3-direct vs TW's 202. Same expression with `hasAttr` is well-behaved. Memory: `project_583_memoization_loop.md`. Memoization on rec-attrset entries is suspect.

### 4.8 Methodology — bisect nixpkgs to distill reproducers; keep them as regression tests

**Rule**: When v3 fails on a real nixpkgs workload, do not get stuck on full-eval debugging. Bisect nixpkgs *itself* (overlays, system, attribute path, by-name slices, commit history, env-gate combinations) to find the smallest input that still triggers the failure. Capture that minimal input as a `.nix` reproducer in `src/libexpr-v3/test/repro-<issue>-<shape>.nix`, wire it into a `run-<issue>-tests.sh` driver, and keep it forever as a regression test.

**Why**: real-nixpkgs failures involve 100+-frame stacks. Manual analysis on the full eval is slow and frequently dead-ends. Reducing to a 5-20 line `.nix` repro gives you (a) something you can run in a tight loop, (b) something you can attach to lang/regression suites, (c) a permanent guardrail against the bug returning under refactor.

**How to bisect** (in order of effort):
1. **Strip overlays**: `(import <nixpkgs> { overlays = []; })` first. If green, the bug is overlay-induced; bisect overlays.
2. **Strip config**: `{ config = {}; }`. If green, the bug is config-induced.
3. **Try different `system` values** (x86_64-linux vs aarch64-darwin) — stdenv bootstrap divergence behaves differently per system.
4. **Use a non-forcing query**: `(import <nixpkgs>)` alone returns a Closure; `builtins.functionArgs (import <nixpkgs>)` works without forcing pkgs. Lets you isolate "is the bug in pkgs construction or in attribute access".
5. **Smallest `pkgs.<attr>` that triggers**: walk down from `pkgs.hello.name` to `pkgs.hello.outPath` to `pkgs.hello.meta.platforms`. Find the *first* attribute whose force triggers the cascade.
6. **Bisect across nixpkgs commits**: `git bisect` on nixpkgs with v3-direct as the test driver. Catches regressions caused by nixpkgs evolution.
7. **Env-gate bisection**: toggle one v3 gate at a time (`NIX_V3_NO_*`) to isolate which v3 mechanism is responsible. Useful gates: `NIX_V3_NO_UPDATE_TAIL`, `NIX_V3_NO_STG`, `NIX_V3_NO_REGISTRY_PEEK` (and whichever survive Phase 0's env-var inventory).
8. **Distill to shape**: once you've found the smallest nixpkgs reproducer, factor out the *pattern* into a synthetic `.nix` (e.g. `lib.fix` + self-dot, `inherit (a) b c` over a with-scope, rec-attrset with a thunk that re-derefs). The synthetic version is the regression test; the nixpkgs version is the smoke test.

**Examples already in tree**:
- `test/repro-455.nix` + `test/repro-455-aliases.nix` — #455 distilled callPackage with-scope shape.
- `test/repro-495-broader-thunkify-bug.nix` — #495 self-dot pattern at the IR level.
- `test/wc38-bisect-harness.sh` + `test/wc38-bisect-README.md` — actual bisection harness for WC-38 with-blackhole bug.
- `test/known-fail-callpackage-with.sh` — captured KNOWN-FAIL state across the symptom shapes #455 → #498 → #516 → #546 → #548.

**Keep them**: do NOT delete a repro after the bug is closed. The repro becomes a positive regression test. Even when v3 evolves past the original cascade, the repro continues to assert the shape works. Memory note: this rule complements `feedback_always_add_tests.md` (every bug fix needs a regression test) — bisecting to find the reproducer is *how* you derive that test for a nixpkgs-class bug.

**Anti-pattern**: "I'll just rerun the full hello.name eval each time" — this is what produces 7-letter RCA cascades. The full eval is the symptom; the bisected repro is the unit you can falsify against.

### 4.10 Sidestep > patch — the Path B / Option 4 hybrid pattern

When a bug is localized to a specific gate or condition, there are two paths to a fix:

(a) **Patch the gate**: modify the failing site directly.
(b) **Sidestep the gate**: redesign the call site so the gate's pathological case is never reached.

**Worked example (Phase 1 close, 2026-05-18)**: Path B's lower.cc gate was localized in commit `ad95edfd2` to one nixpkgs site (`all-packages.nix:7385` `inherit (libsForQt5.callPackage ../development/libraries/wt { }) wt4;`) and one gated thunkification rule in `lower.cc`. The natural (a)-style fix would patch the lower.cc heuristic.

Instead, the actual Phase 1 closure came via the **Option 4 hybrid** wrapper (commit `7adc7e61f`), which wraps `builtins.derivationStrict` / `builtins.derivation` in a bytecode-level iteration via `builtins.foldl'`. The recursion that was blowing the C-stack moved onto v3's frame stack, where Phase 1.2's iterative-force protocol handles arbitrary depth. The lower.cc gate's pathological case (deep C-recursion through stdenv inputs) no longer fires because the recursion lives in v3 frames now.

Why (b) was better here:
- The lower.cc gate's underlying decision (when to thunkify inherit-from expressions) was correct in principle. Patching it would either over-thunkify (perf regression on other patterns) or under-thunkify (correctness elsewhere). The trade-off is fundamental to the eager-vs-lazy asymmetry (LESSONS §1.3).
- The C-recursion blowing the stack was a symptom of TOO MUCH WORK happening per C-frame in the derivation pipeline. Redesigning the pipeline (Option 4) reduced the C-frames per derivation to a small constant; the gate's behavior was no longer load-bearing for stack safety.
- Option 4 also produced an architectural benefit beyond fixing the bug: derivation iteration now runs entirely in bytecode, eliminating one whole class of "TW round-trip per attr" overhead.

**Lesson**: when a gate is localized, evaluate both paths before patching. Sometimes the gate's failure mode is the symptom; the redesign of the call site is the fix. Particularly when the gate has a deep architectural reason to exist (e.g. eager-vs-lazy laziness decisions), patching it patches the wrong layer.

**Anti-pattern warning**: sidestepping is not always available. Don't generalize "sidestep > patch" as a rule. The check is: does the sidestep produce a structurally cleaner state (Option 4 did — derivation runs in bytecode), or does it just paper over the bug elsewhere (an opt-in gate is the bad version of "sidestep")? Real sidesteps reduce total complexity; bad sidesteps add it.

### 4.11 Bytecode-primop installation tradeoff: callback yes, non-callback no

The T0-T17 bytecode-primop installation series (commits `5104a7270` → `537e06460` over 2026-05-17) installed several primops as v3 bytecode. The result, after T13-T17 were reverted in `231c5b393`:

**Installed as v3 bytecode (callback-using; benefit)**:
- `foldl'`, `map`, `filter`, `all`, `any`, `concatMap`, `partition`, `groupBy`.

**Reverted to C implementation (non-callback; no benefit)**:
- `catAttrs`, `concatLists`, `listToAttrs`, `removeAttrs`, `intersectAttrs`.

**The pattern**: a primop benefits from bytecode installation iff:
1. It takes a callback (lambda) that would otherwise round-trip from C-impl → TW eval → return → C-impl. Bytecode installation keeps the callback in v3 frames; eliminates the round-trip.
2. It's hot enough that the round-trip cost dominates the primop body.

Pure C transforms (no callback) get no benefit from bytecode installation — the bytecode wrapper just adds dispatch cost. The body is the same fixed C transform; there's no callback to keep local.

**Lesson**: when considering installing a primop as bytecode, ask "does it have a callback?" If no, leave it in C. If yes, install if it's hot.

**Caveat**: bytecode primops can interact with the per-op overhead factor of the 200× force-rate gap. Installing too many primops as bytecode may itself slow things down by increasing dispatch volume. Bench before scaling.

### 4.9 The debug story (mandatory infrastructure for a VM project)

A bytecode VM that compares against an existing evaluator needs a debug story that's broader than "add a printf when needed." The full story:

1. **Bisect nixpkgs.** See §4.8.
2. **Differential testing against TW as oracle.** Today: `run-cutover-parity-tests.sh`, `run-drv-parity.sh`, `bench-v3-vs-tw.sh` — but only on fixed test suites. Needed: arbitrary-input front door `v3-diff-eval <file.nix> [--mode=output|trace|deep]`. Output mode = string match (catches result bugs). Trace mode = NIX_TRACE_EVAL force-event diff (catches force-order divergence; A9 demonstrated that string match alone can hide 25M extra force events). Deep mode = all of the above plus value-shape diff.
3. **Trace evaluation semantics.** Already: NIX_TRACE_EVAL (force order), `V3_DBG_HOT_FORCE` (per-position counts), `V3_DBG_ALLOC_DUMP` (top-N descriptors), `V3_DBG_HOT_CALLEE` (callCount + stack-dump). Gap: outputs are per-tool. Consolidation target: single `V3_DBG_TRACE=force,alloc,call` style category bitmask (action plan Phase 4).
4. **Regression tests.** §4.8 + CLAUDE.md "every bug fix needs positive + negative + regression tests." Keep every bisected repro forever.
5. **Profiling.** Already: `V3_TIMING` (vm_ms/bridge_ms split), `allocStats`, bench harness (`bench.py`). Gap (being closed, 2026-05-20): documented CPU-profile workflow. Design committed in `PERF_TRACE_TOOL_DESIGN_2026-05-20.md`: `make perf-trace WORKLOAD=...` runs TW + v3-direct under a `psutil` sidecar sampler (CPU% / RSS / process-tree) plus an in-process `GC_get_heap_size()` probe gated by `NIX_V3_HEAP_TRACE`, and emits SVG overlays + JSONL traces in `bench/samples/<date>/`. ~3 days to first measurement; orthogonal to (and not a replacement for) `samply` / `xctrace` flamegraph workflows, which remain the right tool for hot-function attribution. Stage 2's "CPU-profile `pkgs ? lib`" can use either; perf-trace is the cheaper default.
6. **Differential fuzzing / randomized parity testing.** Generate random Nix expressions (grammar-based: `let`/`with`/`inherit`/`rec`/`if`/lambda/attrset, plus small primops), run TW and v3-direct, assert output and trace match. Structural shrinker on failure. Run hourly in CI once stable. Would have caught the eager-vs-lazy asymmetry cascade (#496-#498-#516-#548-#577) earlier than hand-written tests did. Initial implementation: 200-400 LoC of Python wrapping the Nix evaluators.
7. **Property tests for VM invariants.** The invariants hand-written tests can't enumerate:
    - `forceValue(forceValue(x)) == forceValue(x)` (idempotence).
    - Two thunks from the same source position + same inputs evaluate equal (sharing equivalence).
    - Cycle detection is total — any reachable cycle is caught, no false negatives.
    - After scavenge, every Value equals its pre-scavenge counterpart.
    - `Bindings::lookup(name)` after rec-attrset init equals what the let-in body would observe.
   These are the invariants the cell-update cascade violated. A8's iterative-force depth test (`13044c379`) is the first; generalize the framework.
8. **Deterministic stress mode.** The A5/A6 fakeClo corruption was non-deterministic — sometimes manifested, sometimes didn't. Need: `V3_DBG_GC_STRESS` (forced scavenge after every N allocations), `V3_DBG_RECYCLE_STRESS` (adversarial closure-pool free-list rotation), `V3_DBG_ALLOC_SEED` (deterministic randomization). Same input + same seed = same crash, every time. **Prerequisite for Roadmap Stage 3 (nursery default-on).**
9. **Crash artifacts on abort.** When v3 hits `std::terminate` / segfault / `assert`, dump: frame stack with source positions, last-N opcodes, current Value being forced, recent allocation IDs. Today A7 found this gap — depth-2000 abort exited with nothing useful. Implementation: `__attribute__((destructor))` + signal handler; output to `/tmp/v3-crash-<pid>.log` so post-mortem isn't lost.
10. **Issue → fixture manifest.** Single index `src/libexpr-v3/test/REPROS.md` mapping every closed `#N` to its repro `.nix` and `run-<N>-tests.sh` driver. Today 3 `repro-*.nix` + 40+ `run-*.sh` exist but are discoverable only by `ls | grep`. Without an index, fixtures will get orphaned over the long arc of Roadmap Stages 2-8.

**Working principle (debug-story-as-rule)**: when you cannot answer "how would I debug this if it failed silently" with one of the 10 mechanisms above, build the missing mechanism *before* the next investigation. The cost of one piece of debug infrastructure is recovered after the first time it shortens an investigation from a week to a day — which is exactly what NIX_TRACE_EVAL did on 2026-05-14.

---

## Part 5 — Process-level lessons

### 5.1 Investigation discipline > investigation budget

The lode/ archive has 54 docs, many redundant. The pattern of "spawn a new doc per investigation" hides the real cost: subsequent contributors must read 54 docs to know what was tried. Worse — when an investigation succeeds, its doc rarely gets a RESOLVED row appended.

**Codify**: every closed bug appends RESOLVED to its lode/ doc. If the bug was opened-and-closed in <7 days, no lode/ doc should have been written.

### 5.2 Architecture docs that aren't load-bearing rot

`OPTIMIZATION_PLAN.md` (177 KB) was written as a forward-looking plan; much of it has been overtaken by events. If a doc isn't referenced by a current TODO or PR, it should be either RESOLVED, DEFERRED, or DELETED.

### 5.3 The action plan exists to break this pattern

`ACTION_PLAN_2026-05-15.md` codifies these meta-rules. The current document (LESSONS_LEARNED) is the data that justifies them.

---

## Part 6 — How to use this document

- **Before opening a new RCA**: read §1.5 and §3.9. Confirm you are not starting a cascade.
- **Before adding a new env-var**: read §3.8. Add the retirement criterion now.
- **Before claiming a perf win**: read §1.4. Bench it.
- **Before reverting**: read §3.7. Don't reapply without measurement.
- **Before "let's bridge through TW for this"**: read §1.1. Stop.
- **Before shrinking primops.cc**: read §1.2. Classify, don't shrink.
- **Quarterly**: re-read this doc. Append new lessons as they emerge. Move stale ones to an archive section.

---

## Appendix — Commit hashes referenced

For traceability:

- `cf12c1880` — TW-routing retraction memo (V3-NATIVE constraint).
- `60c6a7666` — V3-NATIVE constraint formalization.
- `e8d7c3885` — v3_hook.cc deletion.
- `07a6352c1` and 6 siblings — partialBindingsRegistry deletion (Phase 3.3).
- `1e040415f` — per-lambda OD blacklist (#455).
- `c909caae6` and `c8406413d` — getenv cache wins.
- `cf253b455` — typed BlackholeError.
- `0a480cfb0` — content-keyed in-memory cache.
- `f6bf3fe8d` and 4 siblings — A8 iterative force.
- `d17afe9e1`, `f8539922a`, `975b711f4` — NIX_TRACE_EVAL.
- `1edb2e982` — Phase 5 polarity revert.
- `951bdef57` — v3→TW scalar fast path revert.
- `857227578` — direct-AttrSelect formals revert.
- `2b9295437` + `cfd6a7ef1` — call-hook precompile postmortem.
- `34a59442e` + 4 siblings — Phase 4 fakeClo (no-op).
- `72ad0c9f9` — post-Phase-4 baseline showing zero movement.
- `4a616bbe5` + `666998bd7` — partial-Bindings peek revert/reapply churn.
- `541d4390a` + `224f912e2` — nursery leaf-tag revert/reapply churn.

This list is not exhaustive. Re-run `git log --since="2026-03-01" --oneline | grep -i pattern` to refresh.
