# Measure Twice, Cut Once — 2026-05-23

A perennial operating rule, codified after recognising the pattern
across multiple recent decisions. Closely related to but distinct
from the falsification rule (`feedback_falsification_rule.md`): the
falsification rule is about how COMMITS exit investigations; this
rule is about how INVESTMENTS get committed in the first place.

## 1. The rule

> Before any multi-week design or implementation, run a cheap
> measurement spike against a pre-committed numeric threshold. If
> the measurement falsifies the premise, kill the work and document
> revival conditions. Save the multi-week investment for premises
> that survive the cheap test.

The carpenter's adage condenses it: measure twice (cheap proxy +
threshold), cut once (commit the work, or kill it). The expensive
cut is the multi-week implementation; the cheap measurement
prevents wasting it on premises that don't hold.

## 2. Why this is its own rule, not a corollary of Rule 0

The falsification rule says "every commit must answer what
hypothesis it kills." That's an exit criterion for investigations
already in flight — once you're cutting, you need a falsifier to
stop cutting.

This rule operates ONE STEP UPSTREAM: before you start cutting at
all. The falsification rule answers "when do we stop?". This rule
answers "should we start?"

Both are needed. The team has been disciplined about the
falsification rule since 2026-05-15; "measure twice, cut once"
formalises the discipline the team has ALSO been showing on entry
decisions but hadn't named.

## 3. The protocol

When a multi-week investment is proposed:

### 3.1 State the load-bearing quantitative claim

The premise that justifies the work, expressed as a number.

Examples from the team's history:
- Stage 9: "Per-thunk-body content-addressing produces ≥ 2× cache
  collapse on nixpkgs"
- Stage 5/6: "AttrSelect family is ≥ 10 % of dispatch on
  representative workloads, so PIC investment pays back"
- Materialization retirement / hash-keyed eval cache: "Cache hit
  rate on warm CI is ≥ 50 % so the deserialize cost amortises"
- Formal verification with TLA+: "≥ N concurrent-protocol bugs in
  the past 6 months that TLA+ would have caught"

If the claim cannot be expressed as a number with a threshold, the
work is not yet ripe for "measure twice, cut once" — first reduce
it to a quantitative claim. Vague premises ("PICs should make
attrsets faster") need sharpening before this protocol applies.

### 3.2 Identify the cheap proxy

The full implementation IS the most accurate measurement, but
also the most expensive. The cheap proxy is a measurement that's
directionally sound — i.e., it provides a one-directional bound
that's sufficient to falsify (or support) the premise.

Examples:

| Investment | Cheap proxy | Direction |
|---|---|---|
| Stage 9 (full IR-level ABT-aware dedup) | Bytecode-level hashing | Lower bound on dedup |
| Stage 5/6 (PIC + hidden classes) | Dispatch-budget % of AttrSelect | Upper bound on PIC win |
| TLA+ on cell-update protocol | Audit recent bugs the protocol class would have caught | Sample bound |
| Materialization retirement | Survey haskell.nix `materialized/` directory contents | Existence proof + scale |

The proxy is directionally sound when:
- A **lower bound** is enough if you're trying to falsify a HIGH claim
  (e.g. "dedup is ≥ 2×" — if even the lower bound is < 2×, the
  claim is dead)
- An **upper bound** is enough if you're trying to falsify a LOW claim
  (e.g. "PIC wall-time saving is meaningful" — if the upper bound is
  < 3 % wall, the claim is dead)
- A **sample bound** is enough if you're estimating from a population
  (e.g. "TLA+ would catch ≥ N bugs/year" — sample recent bugs)

If the proxy is two-directional (could go either way) or noisy
(could be off by a factor of 10), it's not yet cheap-enough; refine
until directionally sound.

### 3.3 Pre-commit the threshold

A specific number, not an adjective. The number is committed
BEFORE the measurement is run.

Pre-committing matters because once data is in hand, motivated
reasoning makes thresholds drift. "1.17× isn't that far from 2×"
sounds plausible only when 1.17× is the actual measurement. "Less
than 1.5× kills" sounds reasonable in advance.

The threshold should be calibrated to the work's effort:
- A 1-week investment: threshold can be low (any positive signal)
- A 4-week investment: threshold should be substantial (clear
  positive signal, ≥ 2× ROI vs the next-best alternative)
- A multi-month investment: threshold should be high (transformative
  signal, ≥ 5× ROI or a unique capability nothing else provides)

### 3.4 Run the measurement

The cheap proxy. Effort target: 1-3 days, ideally < 1 week. If
the spike itself takes > 1 week, either the proxy isn't cheap
enough OR the protocol isn't a good fit for this decision.

### 3.5 Apply the threshold

Binary. Either the measurement passes the pre-committed threshold
(commit the work) or it doesn't (kill the work).

If the result is genuinely close to the threshold (within 10 %),
that's not a "let's split the difference" outcome — it's a "the
proxy was wrong-sized for this decision; redesign and re-measure"
outcome. Don't compromise on the threshold; redesign the proxy.

### 3.6 If kill: document revival conditions

The kill must include explicit re-measurement triggers (what data
would change the decision, at what milestone, with what effort).
See `STAGE_9_KILLED_2026-05-22.md` §7.5 and
`STAGE_5_6_KILLED_2026-05-23.md` §7 for examples of the format.

Without revival conditions, "we'll revisit later" becomes either
"never decide" (lode/-proliferation failure mode) or "whenever
someone has a fresh idea" (falsification-rule failure mode).
Revival conditions close both.

### 3.7 If commit: proceed

The work has been justified by data. Proceed with the multi-week
implementation. The falsification rule then takes over (each commit
answers "what hypothesis does this kill?") as the in-flight
exit criterion.

### 3.8 Variant — when the implementation IS the spike

Sometimes no directional proxy is cheaper than the real thing. A
fused opcode, a localised IC, a small dispatch micro-opt — the
*only* way to know if it pays is to build it and measure. In these
cases the protocol changes shape but the discipline is the same.

The variant protocol:

1. **Confirm no cheaper proxy exists.** Be honest about this. A
   one-line counter or a static analysis often beats a full
   implementation; reach for those first. The "implementation is
   the spike" path is the FALLBACK, not the default.

2. **Cap the implementation size.** Target: ≤ 2 days of work and
   ≤ 200 LoC for the minimum viable version. If the optimisation
   can't fit in that envelope, you're not doing this variant —
   you're doing a multi-week investment, which means a cheaper
   proxy must be found first (back to §3.2).

3. **Pre-commit BOTH thresholds.** Not just "keep above X" but
   also "revert below Y." For example: "OP_SET_LOCAL_KEEP keeps
   if wall delta ≥ 8 ms; reverts if wall delta < 3 ms; reassess
   between." The "reassess between" zone exists for borderline
   data; commit in advance to what "reassess" means (typically:
   re-measure on a second workload).

4. **Pre-commit the revert criterion explicitly.** Not just "if
   it doesn't pay, we might revert." A specific data signal and
   an immediate revert if that signal fires. Including: which
   commit will be reverted, what the revert commit message will
   contain, who owns the revert.

5. **Implement the minimum viable version.** No bells, no
   gates beyond what's needed for the measurement. The opt-in
   gate (`NIX_V3_TRY_X=1`) **is not allowed** — the gate
   pattern is exactly what this rule was designed to prevent.
   The implementation goes default-on or it doesn't ship.

6. **Measure.** Same as §3.4.

7. **Apply the threshold binarily.** Three outcomes:

   - **Pass (keep + harden):** the implementation stays; follow-up
     commits harden it (tests, regression coverage, doc).
   - **Borderline (re-measure):** ship to a second workload OR a
     second iteration of the cheap proxy. The "reassess" zone
     resolves to keep or revert; it doesn't become "leave it
     gated."
   - **Fail (revert):** delete the implementation. The revert
     commit must include:
     - The measurement data (numbers, workload, command-line)
     - The pre-committed threshold (so the reader sees the decision
       criterion)
     - A short rationale ("why this didn't pay") so the same
       investigation isn't started fresh in 3 months
     - A reference to any *kept* diagnostic / instrumentation
       (with justification — kept items must have independent
       value beyond the failed optimisation)

The **revert commit IS the falsifier**. It's the artifact that
closes the investigation. Without it, "we tried X" becomes "X is
behind a gate, who knows" — exactly the anti-pattern Rule 0
prohibits.

#### Pivot vs revert

There's a tactical distinction worth naming:

- **Pivot**: the specific variant didn't work; try a different
  variant of the same idea. The Stage 4 v4 → v4.1 → v4.2 → v4.3
  → v4.4 sequence is this pattern. Each iteration was cheap; the
  underlying claim ("strictness analysis pays") is still under
  test. Pivot is fine when (a) the next variant is itself cheap,
  AND (b) the underlying claim hasn't been falsified — just one
  instantiation of it.

- **Revert**: the underlying claim itself is falsified. The work
  is removed entirely. Stage 9 was a revert by another name —
  the L0 spike was the implementation; the kill was the revert.
  Stage 5 / 6 same.

The decision between pivot and revert is itself a falsification
question: does the data invalidate THIS implementation, or does
it invalidate the underlying premise? If the latter, revert; if
the former, pivot (but cap the pivot count — three failed
pivots on the same premise IS a falsification of the premise).

#### Kept-vs-discarded discipline

When a revert keeps some sub-piece (e.g. a diagnostic that
proved useful, an infrastructure piece that has independent
value), the kept item must be explicitly named and justified.
Examples from team history:

- #775 Force(MkThunk) variant — 0 elisions / 10 563 candidates.
  The optimisation was reverted; the funnel diagnostic was kept
  as an inert detector for future iteration. *Justified because*
  the diagnostic detects the pattern at zero cost and informs
  future variants.
- #780 register-VM falsifier — implementation NOT undertaken
  (correct: no cheaper proxy was possible, but the implementation
  is multi-month — exceeds §3.8's 2-day cap). The bigram-matrix
  instrumentation kept as the measurement substrate. *Justified
  because* the matrix supports future super-instruction
  experiments at lower cost.
- Stage 9 L0 spike — the `dedup_survey.{hh,cc}` infrastructure
  kept after Stage 9 kill. *Justified because* it allows
  re-measurement at coarser granularity (one of Stage 9's
  revival triggers).

The kept item must be explicitly load-bearing for SOMETHING
forward-looking — not "we might use it someday."

#### Why this matters

Without the revert discipline:

1. **Optimization carcasses accumulate.** Failed optimisations
   stay in the tree behind opt-in gates. Six months later no
   one remembers if they were tried; the team re-tries the
   same idea OR refuses to revisit it because "it's already
   in there." Both fail modes are bad.
2. **The lode/-proliferation pattern returns.** Each failed
   optimisation becomes a `lode/X_TRIED_2026-XX-XX.md` doc
   without a corresponding revert. Documentation accumulates;
   code doesn't shrink.
3. **The falsification rule is undermined.** Rule 0 explicitly
   prohibits "adding an opt-in gate so both can coexist for
   now" as an investigation exit. Failed-but-gated optimisations
   are exactly this anti-pattern.

The revert commit is short, mechanical, and high-information.
Two lines of measurement data + one sentence of rationale + the
file deletion is sufficient. Don't over-engineer the revert
itself — the value is the *act* of reverting, not a 1000-line
post-mortem.

## 4. Examples from the team's history (positive)

### 4.1 Stage 9 (cell-level content-addressing) — KILLED 2026-05-22

- Claim: per-thunk-body content-addressing produces ≥ 2× collapse on
  nixpkgs
- Proxy: bytecode-level hashing (lower bound on IR-level)
- Threshold: ≥ 2× function-level dedup
- Measurement: 1.17× function-level / 1.03-1.07× byte-level
- Decision: kill
- Effort saved: ~5 weeks of L1-L4 + ABT refactor + maintenance
  debt
- Effort spent on proxy: ~1 day
- Revival conditions: documented in `STAGE_9_KILLED_2026-05-22.md`
  §7.5

### 4.2 Stage 5 / 6 (hidden classes + PICs) — KILLED 2026-05-23

- Claim: AttrSelect family is ≥ 10 % of dispatch, justifying PIC
  investment
- Proxy: dispatch-budget profiling (upper bound on PIC ceiling)
- Threshold: ≥ 10 % of dispatch
- Measurement: 2.24 %
- Decision: kill (both stages)
- Effort saved: ~12 weeks of design + implementation
- Effort spent on proxy: ~half a day (added one line to
  NIX_VM_OPCOUNTS banner)
- Revival conditions: documented in `STAGE_5_6_KILLED_2026-05-23.md`
  §7

### 4.3 Phase D nursery write barriers — COMMITTED 2026-05-21

- Claim: per-container dirty bits (Path α) is correct + sufficient
- Proxy: 15-test stress suite under STRESS=1000
- Threshold: 15/15 PASS
- Measurement: 15/15 PASS
- Decision: commit, default-on (`NIX_V3_PHASE_D=1`)
- Effort spent on Phase D: ~6 hours (decision memo → milestone)
- This is the positive-outcome version of the pattern: measure,
  threshold met, commit, no kill needed

### 4.4 Materialization retirement (Unison Item 3 narrow scope) — DEFERRED 2026-05-21

- Claim: content-addressed eval cache replaces haskell.nix
  materialization
- Proxy: existing materialized/ directory contents prove the
  pattern works AT SOURCE LEVEL; v3-cache lifts it into the
  evaluator
- Threshold: implementation cost ≤ 4 weeks AND clear win on warm
  CI
- Measurement: implementation cost estimate is 1-2 weeks; warm-CI
  win quantification deferred to first prototype
- Decision: deferred (queued behind Stage 4 strictness work) but
  fully scoped with phased plan in `IFD_DEEP_DIVE_2026-05-21.md`
  §11
- This is the "measure twice, NOT YET CUT" version: the proxy
  supports the work, but other work is sequenced higher

## 5. Anti-patterns (negative examples — what to avoid)

### 5.1 "Let's just try it and see"

No. State the success criterion in numbers BEFORE the spike. "Try
and see" makes the threshold a post-hoc justification.

### 5.2 "We already know X is hot / cold / fast"

Measure it anyway. Intuitions about VM behaviour are often wrong.
The #702 falsifier killed the "GC-scan dominance" hypothesis the
team had been operating under for weeks. The #758 falsifier showed
v3-native callFlake was achievable in hours after months of
"this is the hard part." The dispatch-budget revealed that
AttrSelect — long assumed to be hot — was only 2.24 %.

The cost of measuring something you "already know" is ~1 day. The
cost of being wrong about something you "already know" is multiple
weeks.

### 5.3 "The proxy isn't precise enough"

A directionally sound proxy doesn't need to be precise. It needs
to clearly land above or clearly land below the threshold. If the
proxy is in the noise band (off by < 50 % from the threshold),
that's the proxy's problem — redesign it. Don't water down the
threshold to fit a noisy proxy.

### 5.4 "The threshold should be set based on the result"

No. The threshold is committed BEFORE the measurement is run.
Post-hoc threshold adjustment is the meta-failure mode this rule
is specifically designed to prevent. If you find yourself wanting
to adjust the threshold after seeing the data, you've discovered
either:
- The threshold was wrong (in which case: state the new threshold,
  re-commit it, re-justify it from first principles, then re-measure
  with the new threshold). Treat this as starting over, not as
  "tweaking."
- The measurement was wrong (in which case: redesign the proxy and
  re-run).
- Motivated reasoning is engaging (in which case: stop, defer the
  decision by a day, return tomorrow and apply the original
  threshold).

### 5.5 "We don't have time to do the spike first"

This is the most insidious version. The argument is "the work is
urgent, so we should skip the cheap measurement and start the
expensive implementation."

The math: if the spike takes 1 day and the implementation takes
4 weeks, the spike is 1/20th of the implementation cost. If the
spike kills the work, that's 1 day spent vs ~4 weeks reclaimed.
If the spike supports the work, that's 1 day spent + 4 weeks
implementation = 4 weeks 1 day. The downside of skipping the spike
is large; the upside (saving 1 day) is small.

The "no time" framing is almost always a leading indicator that
the work is being motivated by something other than the data. Pause
specifically when this argument arises.

### 5.6 "The proxy ALREADY exists; this isn't a new measurement"

Sometimes the proxy already exists (e.g. NIX_VM_OPCOUNTS, V3_TIMING,
allocation stats). In that case the protocol is even cheaper —
hours instead of days. But still pre-commit the threshold. The
trap when reusing existing instrumentation is reading the data
through the lens of the proposed work rather than against an
independent threshold.

### 5.7 "The measurement says A; therefore A is true"

**Methodology audit before structural conclusion.** When a measurement
produces a result that would justify a major architectural pivot
(kill a stage, declare a lever too small, conclude an entire scope
class is wall-bound), audit the methodology BEFORE applying the
falsifier.

The trap: measurement infrastructure is itself a system, and that
system can have bugs. The bug doesn't show up as "wrong number" — it
shows up as "the number is correct but answers a different question
than you thought." Catching this requires asking, before declaring a
structural conclusion: *what's the simplest alternative explanation
for this number? Is the measurement actually measuring what I think
it's measuring?*

**Empirical justification — two false-structural conclusions in one
week (2026-05-23 / 2026-05-24):**

1. **Phase 4b cache scope** (`35564703f` RCA). Initial measurement:
   Phase 4b wall-neutral across multiple workloads. Initial structural
   conclusion: "lever too small at primop-call boundary; need to
   move to coarser scope." True cause: cache hook ran on every
   `import` including nixpkgs-internal lazy imports, not just IFD
   imports. RCA-fix → 1.10× faster on designed workload.

2. **CU-disk-cache cold-tax artifact** (`fe678273a` / `297f900971`
   RCA). Initial measurement: Phase 4b COLD +78 % wall tax. Initial
   structural conclusion: "real cold-write tax that scales with cache
   work." True cause: `hyperfine --prepare "rm -rf <cache>"` wiped
   the default-on CU disk cache; the cost was CU recompile, not
   Phase 4b. Correct methodology → COLD === OFF within noise.

**Both errors had identical shape:**

> Profiling told us A.
> Structural conclusion required A AND a hidden B.
> B turned out to be where the cost actually lived.

**Operational addition to the rule:** when a measurement crosses a
falsifier threshold — particularly when it would justify abandoning
a multi-week direction — perform a methodology audit BEFORE
publishing the structural conclusion. The audit asks:

- What ELSE could be producing this number?
- Are there hidden interactions with adjacent caches / instrumentation /
  test harness that I'm not measuring?
- Is the timing envelope scoped to the system I think I'm measuring?
- If I deliberately broke the measurement (e.g. by changing test
  harness setup or instrumenting a different layer), would I see the
  same number?

If a methodology audit is impossible (no time, no second observer,
no alternative instrument), default to "measurement insufficient to
declare structural conclusion." Treat the falsifier as suspended
until the methodology question is closed.

The cost of a methodology audit is usually ≤ 1 hour. The cost of a
false structural conclusion can be days (the team's eval-cache
architecture doc and §13 amendment chain were partially shaped by
the Phase 4b false-neutral reading; the doc had to be amended twice
across consecutive commits).

**See also:** [`PROFILING_AUDIT_2026-05-24.md`](PROFILING_AUDIT_2026-05-24.md)
§4 (the two RCAs as evidence) and
[`PROFILING_IMPROVEMENTS_2026-05-24.md`](PROFILING_IMPROVEMENTS_2026-05-24.md)
T1.1 + T2.3 (the specific instrumentation that would have caught both
errors at first measurement instead of after structural commitment).

## 6. When the rule does NOT apply

This rule is for **multi-week investments**. It does not apply to:

- **Small fixes** (< 1 week): just do them. The cost-benefit math
  doesn't favour pre-measurement.
- **Bug fixes**: the bug is the measurement. You don't pre-measure
  before fixing a known bug.
- **Cleanups / hygiene**: env-var hygiene, dead-code retirement,
  comment fixes — these are unconditional good and don't need
  threshold-based justification.
- **Required correctness work**: V3-NATIVE TW-fallback fixes, parity
  bugs with TW, semantic correctness — required, not optional.

The rule applies when:
- The work is large (multi-week)
- The premise is quantitative (X is hot, Y dedups, Z accelerates)
- A cheap directional proxy is constructible
- The decision is reversible (we could choose either way without
  blocking critical-path work)

## 7. Why this works specifically for v3

Three reasons the pattern fits this project unusually well:

1. **The v3 codebase has rich, cheap diagnostics already.** Most
   measurements the team needs can be expressed as additions to
   `NIX_VM_STATS=1` or `V3_DBG_*` gates, often in ~50-200 LoC.
   The cheap proxy isn't conceptually expensive in this codebase.
2. **Differential testing against TW provides a natural threshold
   substrate.** "v3 matches TW byte-for-byte on X workloads" is a
   measurable, threshold-able claim that v3 work can be evaluated
   against. The team has built this discipline.
3. **The team's velocity makes the math worse for skipping spikes.**
   When the team can ship 14 commits in an afternoon, a 1-day
   spike is genuinely cheap. The alternative (multi-week
   implementation that turns out to be unjustified) wastes more
   calendar than slower teams would lose to the same mistake.

These three together mean the protocol's overhead is small and its
upside is large. Other projects might choose differently; for v3
the rule fits.

## 8. Adoption

This rule applies retroactively to ongoing work:

- Active stages should have a load-bearing quantitative claim
  identifiable. If not, sharpen the claim or downgrade the stage to
  "premise-needs-clarification" status.
- Active design docs (`LINT_INFRASTRUCTURE_DESIGN_2026-05-22.md`,
  `IFD_DEEP_DIVE_2026-05-21.md`, etc.) should expose their
  underlying quantitative claims at the top of their text.
- Future stages added to `ROADMAP_TO_VISION` should include the
  measurement-and-threshold protocol in their initial doc, not
  as an afterthought.

This rule also applies prospectively to the local-stack-motion fix
that's currently the largest unscoped lever (per
`ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md`):

- Claim to measure: "Register-VM (or super-instructions / threaded
  code) reduces dispatch overhead by ≥ X %"
- Cheap proxy: prototype the alternative VM shape on ONE hot loop
  (foldl' from `LESSONS_LEARNED §4.9`) and measure dispatch-cycle
  delta
- Threshold: ≥ 2× dispatch reduction on the prototype
- Effort: 1-3 days for the proxy
- Decision rule: commit if ≥ 2×; kill if < 1.5×; redesign-and-
  re-measure if between

If the local-stack-motion work is approached without this
protocol, it joins the pre-2026-05-22 risk pattern. Approached
with it, it gets the same Rule-0 discipline that just killed
Stages 5/6/9.

## 9. References

- The upstream rule: `feedback_falsification_rule.md` — every commit
  must answer "what hypothesis does this kill?" Measure-twice-cut-
  once is the entry-decision counterpart of that exit-decision rule.
- Recent positive applications:
  - `STAGE_9_KILLED_2026-05-22.md` — bytecode-dedup spike + kill
  - `STAGE_5_6_KILLED_2026-05-23.md` — dispatch-budget spike + kill
  - `FORMAL_VERIFICATION_ANALYSIS_2026-05-22.md` — TLA+ scope
    analysis using "audit recent bugs as cheap proxy" pattern
  - Phase D nursery (#720) — STRESS=1000 → 15/15 PASS as threshold
    measurement before default-on flip
- Project culture context: `LESSONS_LEARNED_2026-05-15.md` Part 0
  identifies the "investigation-without-convergence" pattern this
  rule helps prevent by sharpening entry-decisions.
- Roadmap policy mechanism: `ROADMAP_TO_VISION_2026-05-15.md`
  "Killed-stage revival triggers" section gives the post-kill
  documentation pattern; this rule is the pre-kill pattern.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
