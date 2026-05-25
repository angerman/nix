# Roadmap Progress Snapshot — 2026-05-23 (Week 1 day 8)

Snapshot of progress against `ROADMAP_TO_VISION_2026-05-15.md`, captured
8 days after the roadmap was authored. Supersedes `ROADMAP_PROGRESS_SNAPSHOT_2026-05-22.md`
which is now historically interesting but obsolete on Stage 2 / Stage 9.

**Headline changes since the 05-22 snapshot:**

- ✅ **Stage 2 closed positive** at 14:24 yesterday (commit `3af813638`,
  `NIX_V3_SKIP_INSTALLABLE_PREEVAL` retired). The meta-kill criterion
  is resolved; "v3-direct as primary" is vindicated.
- ✗ **Stage 9 killed by Rule 0** at 22:37 yesterday (commit `37616ecc6`,
  bytecode-dedup survey 1.17× function / 1.03-1.07× byte vs 2× kill
  threshold). ~5 weeks of L1-L4 work cancelled. See
  `STAGE_9_KILLED_2026-05-22.md` for the falsifier data.

Together these are the project's most consequential single day so far.

## Stage status table

```
                              Planned     Actual      Status
Stage 1  Action plan          W1-W8       ~80-90%     █████████████████░  IN PROGRESS
Stage 2  Pure-bytecode eval   W9-W14      ✅ CLOSED   ████████████████████ CLOSED POSITIVE ✅
Stage 3  Nursery default-on   W15-W20     ~95%        ███████████████████ ESSENTIALLY DONE ✅
Stage 4  Uniform STG-shape    W21-W28     ~40-50%     ████████░░░░░░░░░░  IN PROGRESS (v4.3 cross-fn strictness)
Stage 5  Hidden classes       W29-W34     ~0-5%       ░░░░░░░░░░░░░░░░░░  NOT STARTED
Stage 6  Polymorphic ICs      W35-W40     ~0%         ░░░░░░░░░░░░░░░░░░  NOT STARTED
Stage 7  Selector thunks      W41-W44     ~10-15%     ██░░░░░░░░░░░░░░░░  PARTIAL
Stage 8  Thin FFI (parallel)  W9-W44      ~35-45%     █████████░░░░░░░░░  IN PROGRESS (disk-cache + dedup_survey)
Stage 9  Module linking       (post-perf) ✗ KILLED    [stage cancelled]   KILL CRITERION FIRED ✗
─────────────────── UX pillars ──────────────────────────────────────────────
Stage 14 Error UX             post-perf   ~10%        ██░░░░░░░░░░░░░░░░  DESIGN + early parity work
Stage 15 Profiler UX          post-perf   ~35-40%     ███████░░░░░░░░░░░  Phase 0-3 substrate landed; #769 lane informs Phase 4
Stage 17 Pattern lint UX      post-perf   ~5%         █░░░░░░░░░░░░░░░░░  DESIGN ONLY
─────────────────── Candidates (pending measurement) ────────────────────────
Stage 10 Salsa                              candidate
Stage 11 HAMT                               candidate
Stage 12 JIT                                candidate
Stage 13 Multi-core capabilities            candidate
Stage 16 Whippet GC                         candidate
```

## Yesterday's key milestones (the consequential ones)

```
✅ 2026-05-22 11:41  #758 callFlake Stage 2 closure — default-on, sweep, bridge retired
✅ 2026-05-22 12:19  #759 nixpkgs drvPath sweep — 63/64 byte-identical, 0 divergences
✅ 2026-05-22 12:51  #701 Phase 4b emitTreeAttrs port — last per-node TW bridge gone
✅ 2026-05-22 14:24  #760 STAGE 2 BINARY EXIT — SKIP_PREEVAL retired       ★★★
✅ 2026-05-22 16:26  A12b primops.cc::valueEqual + forceDeep iterative
✅ 2026-05-22 17:01  #745 Stage 4 v4.3 cross-function strictness analysis
✅ 2026-05-22 17:35  #762 opt_strictness PrimOpCall whitelist + cross-block
✅ 2026-05-22 18:08  #764 SKIP_INSTALLABLE_PREEVAL final scrub (silent no-op)
✅ 2026-05-22 22:37  #772 STAGE 9 PHASE L0 SPIKE — KILLS STAGE 9            ★★★
```

24 commits over the day, including 8 marked-time hours of post-Stage-2-closure
work. The afternoon shifted from architectural-binary to depth-cleanup.

## Bench landmarks (where v3 stands today)

```
Workload                  Today                        Notes
───────────────────────────────────────────────────────────────────────
hello.name                ~1.4× TW                     Phase 1 exit (unchanged)
hello.drvPath/outPath     ~30× TW peaked at 15.7×      Memory −849 MB peak
                          v3-native cold path           (PosIdx inlined; #752)
cardano-node M5           ~2.0× TW wall                v3-NATIVE this time
                          byte-identical to TW          (bridge retired in #758)
                          (was 1.28× with bridge)
nixpkgs drvPath sweep     63/64 byte-identical          stdenv + build tooling
                          0 divergences                 (vlc skip: TW also fails)
```

The cardano-node M5 number went UP (1.28× → 2.0× TW) when v3-native
callFlake replaced the bridge. That's expected — the bridge was
delegating work to TW; v3-native does it all in v3 and is currently
slower at that part. Stage 4 + Stage 5 are the perf-recovery path
for this number.

## Stage 2 closure (the headline)

Per the roadmap (lines 807-815):

> At end of Stage 2 (≈ Week 14, ≈ 2026-08-21): if
> `NIX_V3_SKIP_INSTALLABLE_PREEVAL` cannot be deleted — i.e. v3-direct
> cannot evaluate real workloads without TW pre-eval — the architecture
> choice "v3-direct as primary" is wrong.

Yesterday at 14:24 (Week 1 day 7), commit `3af813638` deleted the gate.
`libcmd/installables.cc:493` no longer calls `state->eval(e, *vFile)`
under v3-direct mode; only `vFile->mkThunk(...)` runs. TW parses; v3
evaluates. 26 test scripts + bench/perf-trace.py stripped the env-var
setting. CLAUDE.md note demoted to historical reference.

**The architectural meta-kill criterion is resolved POSITIVE.** v3
owns evaluation. Stages 3-7 are now unambiguously the work to do
(they only made sense if Stage 2 closed; it did).

Stage 2 closed **13 weeks early** vs the Week 14 deadline. My 2026-05-22
AM estimate said realistic closure was Week 3-4; the actual closure was
day 7. **I was off by 2-3 weeks; the team was off the roadmap by 13.**

## Stage 9 kill (the other headline)

Per the roadmap kill criterion:

> "If L0's nixpkgs dedup survey shows <2× collapse, the per-thunk-body
> granularity hypothesis is wrong; abandon Stage 9 and revisit at the
> whole-`ExprAttrs`-or-`ExprLet`-bindings level."

Measured: 1.17× function-level / 1.03-1.07× byte-level on two
independent workloads (hello.drvPath + cardano-node M5). Decisively
below the 2× threshold. Stage 9 (5 weeks scoped) cancelled.

**This is the cleanest Rule 0 outcome to date.** A pre-committed kill
criterion existed in the roadmap; a cheap L0 proxy spike measured the
falsifying data; the stage was abandoned before the L1-L4 investment.
Compare to a hypothetical where Stage 9 was attempted without an L0
spike — ~5 weeks + ~1 500 LoC + maintenance debt before discovering
the hypothesis was wrong.

Consequences (per `STAGE_9_KILLED_2026-05-22.md`):
- #773 (ABT alpha-equivalent identity refactor) demoted to Stage 5
  prereq only; no longer Stage-9-justified
- L1, L2, L3, L4 cancelled (~4 weeks reclaimed)
- Per-file disk cache (existing infra) becomes the architectural
  caching choice
- `LINKING_DESIGN_2026-05-17.md` should be marked SUPERSEDED
- ROADMAP_TO_VISION needs updating (Stage 9 removed from stage list)

## Revised timeline projection

```
                            Original Plan   Actual / Realistic
─────────────────────────────────────────────────────────────────
Stage 1 complete            Week 8          Week 2-3
Stage 2 complete            Week 14         ✅ Week 1 day 7 (done)
Stage 3 complete            Week 20         Week 2 (essentially done)
Stage 4 complete            Week 28         Week 4-8 (v4.3 in progress; v4.4 next)
Stage 5 complete            Week 34         Week 8-16
Stage 6 complete            Week 40         Week 16-24
Stage 7 complete            Week 44         Week 20-28
Stage 9                     (5 weeks)       ✗ CANCELLED — calendar reclaimed
Perf-stage end-state        2027-Q1         2026-Q4 plausible
UX pillars (14/15/17)       2027-Q3         2027-Q1 plausible
```

The Stage 9 cancellation directly reclaims ~5 weeks. Combined with the
team's sustained 3-5× velocity, **end-state target of "v3 at end of
Stage 7 + Stage 14" plausibly shifts left by 3-4 months** vs the
original 2027-Q1 target.

## What changed about prior assessments

I've been wrong on three predictions in the last 36 hours, all in the
same direction (too conservative):

1. **2026-05-22 AM**: "Stage 2 closure realistic ~Week 3-4." Actual:
   Week 1 day 7. Off by ~3 weeks.
2. **2026-05-22 AM, formal-verification framing**: "Stage 2 work is
   research-shaped and won't accelerate like plumbing." Actual: most
   of it WAS engineering-shaped; the team executed in hours what I
   predicted in weeks. Only the A12 memoization fix remained as
   genuinely research-shaped and was either resolved indirectly or
   tolerable enough at runtime that the gate was deletable without
   a specific fix.
3. **My Stage 2 "research-shaped runway is tighter than calendar
   suggests" framing**: Wrong direction. The team's velocity advantage
   extends further into architecture-shaped work than I'd estimated.

Pattern: **I'm consistently under-estimating the team by 2-5×.** Future
estimates should bias toward "the team will execute faster than I
predict" rather than padding for safety.

That said, **Stages 5 / 6 are still genuinely research-shaped** (V8-style
hidden classes, PIC dispatch) and may not accelerate the same way. The
data on this is what Stage 4's strictness work produces over the next
2-4 weeks: if Stage 4 closes faster than the v4.3 → v4.4 trajectory
suggests, then my Stages 5/6 estimates are also too conservative.

## Active investigation lanes

After the morning's Stage 2 push, the afternoon's commits show three
parallel lanes active:

1. **Stage 4 strictness** (#745, #762, #766a, #774): v4.3 cross-function
   landed; v4.4 through higher-order callees is next-week candidate.
2. **Disk-cache productionisation** (#769, #770, #770b, #770c): per-import
   timing breakdown; skip-parse on hit; falsifier; cu.symbolTable
   serialisation trimmed. Now the *only* caching strategy after Stage 9
   cancellation, so getting it right matters more.
3. **Gate hygiene** (#767a, #768a/b): magic-static env-vars promoted to
   `namespace inline const bool`. ACTION_PLAN env-var-hygiene target.

A12b (helper-level iterative forceValue) is essentially closed — the
remaining recursive call sites swept yesterday at 16:26.

## What's NOT closed (the next named blockers)

- **Stage 4 strictness producing real elisions** — v4.3 cross-fn landed
  but the headline "0 elisions on benched workloads" pattern from v4.2
  needs to be definitively reversed. Next measurement after v4.4.
- **Disk-cache deserialise cost (1.78 ms/file)** — currently wall-clock
  neutral on hello.drvPath. Either optimise or accept compile as a
  share of eval cost.
- **hello.drvPath perf gap (~15.7× → close to TW)** — the v3 force-rate
  decomposition's factors b and c (GC scan + intermediate allocs)
  remain partly unmeasured. Stage 3 nursery default-on closed factor b
  (the GC factor); factor c needs Stage 4 strictness wins to close.

## Recommended next moves

Given Stage 2 + Stage 9 closure overnight:

1. **Update ROADMAP_TO_VISION**: mark Stage 9 cancelled; reflect Stage 2
   closure in the Appendix; update the Effort Summary table.
2. **Mark `LINKING_DESIGN_2026-05-17.md` SUPERSEDED** pointing at
   `STAGE_9_KILLED_2026-05-22.md`.
3. **A12 memoization investigation**: even though SKIP_PREEVAL was
   deletable, the original A12 symptom (parse.nix:61 11.5M forces vs
   TW 202) may still be hot. Worth measuring under v3-native to see if
   it now matters less or still does.
4. **Stage 4 v4.4** (cross-fn strictness through higher-order callees):
   multi-day implementation; the team's named next item.
5. **Disk-cache deserialise optimisation**: 1.78 ms/file is the
   ~80% of post-Stage-9 caching ROI.
6. **Stage 5 preliminary design** (hidden-classes / shapes): no commit
   pressure; quietly start the design doc while the team is at high
   velocity.

## Honest read

This is the strongest single day of progress in the project's history.
Stage 2 + Stage 9 in twelve hours; both were potentially-multi-week
items in the original plan; both resolved decisively. **The
falsification rule earned its keep yesterday twice** — once positively
(Stage 2 closed because the work converged, not because we lowered the
bar) and once negatively (Stage 9 killed because the data didn't
support the hypothesis).

The risk profile of the project has shifted substantially. Before
yesterday: Stage 2 was the meta-kill-criterion gate and the highest
project-level risk. After yesterday: Stage 2 closed positive, Stage 9
removed, calendar reclaimed. The next risk-shaped items are deeper
in the roadmap (Stage 4 strictness wins; Stages 5/6 architecture).

If the team's velocity holds through Stage 4 — the next stress test —
the end-state target shifts forward by 3-4 months. That's not a
prediction; that's "what the calendar reads if the trajectory
continues." The pattern of the last 36 hours is that I should bias
toward this being possible rather than padding for safety.

## Cross-references

- Yesterday's now-superseded snapshot: `ROADMAP_PROGRESS_SNAPSHOT_2026-05-22.md`
- Stage 9 kill memo (full falsifier data): `STAGE_9_KILLED_2026-05-22.md`
- Formal verification analysis (background for the kill discipline):
  `FORMAL_VERIFICATION_ANALYSIS_2026-05-22.md`
- Lint infrastructure (Stage 17 design): `LINT_INFRASTRUCTURE_DESIGN_2026-05-22.md`
- Original roadmap: `ROADMAP_TO_VISION_2026-05-15.md` (needs Stage 9
  removal + Stage 2 closure annotation)

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
