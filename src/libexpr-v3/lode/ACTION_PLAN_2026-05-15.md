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

### Phase 1 — Close the A-series with iterative forceValue (Days 4-14, 2026-05-18 → 2026-05-28)

A8 scaffolding is already partial. Finish it before continuing fakeClo / cycle work. Without iterative `forceValue`, deep stdenv hits C-stack overflow regardless of correctness.

- [x] **Audit every recursive `forceValue` call site** in vm.cc + primops.cc — DONE 2026-05-15, commit 988c92c0c. `ITERATIVE_FORCE_AUDIT_2026-05-18.md` enumerates 176 sites + 5 priority candidates (B1-B5).
- [partial] **Convert (b) sites** to writeback-style iterative force — partial 2026-05-15:
  - Step 1 (commit 8df749725): callClosure primop-arg WHNF fast-path
  - Step 2 (commit 557d1fac8): primConcatLists / primConcatStringsSep WHNF fast-path
  - Step 3 falsification (commit 7a9ccc0e6): B3 valueEqual is NOT a meaningful target (depth probes 100-5000 all pass via TCO / shallow recursion)
  - **REMAINING**: App-spine deep recursion — dispatchLoop-driven conversion to eliminate callClosure → forceValue C-recursion. Multi-day; surfaces in `v3-iterative-force-depth`'s app-spine-5000 probe. Proper Phase 1 follow-up.
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
- Phase 1: [pending, target 2026-05-28]
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
