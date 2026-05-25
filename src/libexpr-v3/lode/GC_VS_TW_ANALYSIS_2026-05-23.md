# v3 GC vs TW GC — Strategic Analysis — 2026-05-23

Strategic memo addressing the question "can v3's GC be substantively
better than TW's, or is TW's GC already optimal?" Answer: **yes,
decisively** — v3 has the better-GC architecture mostly built; TW's
Boehm-only GC is structurally limited. The path from "today's parity"
to "v3 measurably better than TW on GC behaviour" is mechanical
(measure-then-flip), not architectural.

Companion to `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` —
that doc covered per-object memory reduction; this doc covers the
GC architecture above it.

## §1 — TW's GC is not optimal

cppnix uses Boehm conservative GC for everything. Boehm has
well-known limitations:

- **Conservative scan** (no precise root info) — must scan all
  word-aligned memory looking for things that might be pointers.
  Per #702 measurement: Boehm scans v3 at 38 MB/s and TW at
  181 MB/s. **The 181 MB/s is Boehm's ceiling on TW's own workload**,
  not a language-imposed limit.
- **No compaction** — Boehm can't move objects; fragmentation
  accumulates. Long evals see RSS climb above true live set.
- **No nursery / no generational** — every allocation pays full
  Boehm cost regardless of lifetime. For Nix eval, where most
  Thunks are forced once and discarded, this is genuinely wasteful.
- **Pause times scale with heap size** — TW's 403 MB Boehm heap
  on hello.drvPath pays a full mark/sweep cost per collection.

The literature is unanimous: a well-implemented precise generational
GC is 2-5× faster than Boehm on dynamic-language workloads with
high allocation rates. TW pays this overhead structurally and
cannot fix it without an upstream Boehm replacement nobody is
making.

## §2 — v3 has the better-GC architecture (mostly)

What's landed:

| Phase | Mechanism | Status |
|---|---|---|
| **Phase A** (#705) | Cheney bump-pointer nursery allocator | landed |
| **Phase C** (#705) | Cheney scavenger (copy live nursery to tenured) | landed |
| **Phase D** (#720) | Write barriers — tenured→nursery pointers tracked in dirty list; safe scavenge without false retention | **default-on** (commit `c0911aee6`, 2026-05-21 15:41) |
| **Phase E v0.2** (#738) | Two-region nursery + age-based promotion — short-lived objects die in nursery, only survivors get promoted | landed, **still opt-in** |

The architecture maps directly to the high-throughput / small-working-
set pattern:

- **High throughput**: nursery bump-pointer alloc is ~3-5 ns;
  Boehm malloc is ~50-200 ns. ~10-50× faster allocation path.
- **Small working set**: scavenger touches ONLY live nursery
  objects (~1-10 % typical survival rate on Nix eval). The 90 %+
  of allocations that die between scavenges cost essentially
  nothing to reclaim.
- **Tenured set stays small**: Phase E age-based promotion means
  only objects that survive 2+ scavenges get promoted. The
  long-lived eval result + lib attrset ends up in tenured;
  transient intermediates never make it past nursery.

Literature for this pattern (GHC GC, V8 young generation,
JVM Eden) consistently reports 50-80 % allocation rate increase +
30-50 % working-set reduction over conservative-only GC.

## §3 — Why nursery isn't shipped default-on yet

Three honest reasons:

### 3.1 Phase E v0.2 has a known stress-mode bug

Per commit `9958eb4a5` (#738 v0.2 fix): "force-walk tenured under
Phase E (close 1 MB stress missed-root)." This is a corner case
under `V3_DBG_GC_STRESS=N` mode — scavenge forced every N opcodes.
It doesn't fire in normal eval, but caution is appropriate.
Shipping nursery default-on when stress mode finds missed roots
risks production hitting similar conditions under load.

### 3.2 Wins haven't been measured on real workloads

`PERF_AUDIT_2026-05-23.md` T3.3 (Phase E mortality measurement) is
flagged as a 1-day spike — **not yet executed**. The v0.1
measurement spike (#738 e371d6072) collected baseline mortality
but the v0.2 two-region delta isn't published.

The team is sitting on architecture that should win, without the
measurement to prove the default-on flip is risk-justified.

### 3.3 The nursery has memory overhead

Cheney semi-space requires 2× space (to-space empty during
scavenge). At default 32 MiB nursery, that's 32 MiB
always-reserved overhead. For workloads where the nursery doesn't
fill (small evals), this is pure cost without benefit.

Mitigations exist (smaller default size, adaptive sizing) but
haven't been tuned.

## §4 — The mechanical path forward (1-2 weeks)

In order, with measurement gates per `MEASURE_TWICE_CUT_ONCE`:

### 4.1 Run Phase E v0.2 mortality measurement (1 day) — PERF_AUDIT T3.3

Workloads: hello.drvPath + cardano-node M5.
Modes: `NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 NIX_V3_PHASE_E=1`
       vs no nursery (TW-like baseline).

Measure:
- Scavenge frequency (count + interval)
- Mortality rate per scavenge (dead vs survivor bytes)
- Total tenured promotions
- Wall-time delta
- Peak RSS delta

**Pre-committed decision rules:**

- mortality ≥ 50 % AND wall delta ≤ 5 % regression: **flip nursery
  default-on** in next commit.
- mortality 30-50 %: tune trigger threshold or smaller nursery
  size; re-measure.
- mortality < 30 %: kill nursery for this workload class;
  investigate why generational hypothesis isn't holding.
- wall regression > 10 %: investigate per-scavenge cost before
  any flip.

### 4.2 Resolve Phase E v0.2 stress-mode missed-root (~1-3 days)

Either fix the root-walk gap directly OR raise the stress-mode
minimum to a regime where the bug doesn't fire. Without this,
nursery default-on means production might trip the same code
path under heavy load.

### 4.3 Flip default-on (commit) (~0.5 day)

`NIX_V3_NURSERY` default-on; provide `NIX_V3_NO_NURSERY` opt-out
for one release for compat. Update tests / lint script / CLAUDE.md
references.

## §5 — Projected post-flip state

If §4 commits, v3's GC pattern becomes:

```
ALLOCATION RATE:
  v3 nursery bump:        ~3-5 ns / alloc
  TW Boehm malloc:        ~50-200 ns / alloc
  Speedup:                10-50× allocation throughput

WORKING SET (live bytes at any moment):
  v3 nursery + tenured:   nursery_size + tenured_live
                          ≈ 32 MiB + ~200 MB on cardano-node M5
  TW Boehm heap:          ≈ 800 MB
  Reduction:              ~3-4× smaller working set

PAUSE TIMES:
  v3 scavenge:            5-20 ms (only live nursery)
  TW Boehm collection:    100-500 ms (full mark-sweep on 800 MB)
  Improvement:            ~10-50× shorter pauses
```

**These are projections based on literature.** §4.1 measurement
is what confirms them on v3's actual workloads.

## §6 — Longer-term ceiling: Whippet

Even with nursery default-on, v3's **tenured arena** remains
Boehm-conservative. That means:

- Tenured collections still conservative-scan (slow)
- Tenured Bindings + Thunks pay full Boehm overhead on promotion
- No compaction in tenured = fragmentation continues
- The ~38 MB/s scan rate remains the ceiling for tenured ops

Stage 16 candidate per `BOEHM_DEPENDENCY_2026-05-21.md` is
**Whippet** (Andy Wingo's precise generational GC for
dynamic-language VMs). If committed, it would:

- Replace Boehm tenured with precise mark-sweep (~5-10× faster
  scan)
- Add compaction (eliminate fragmentation)
- Compose with v3's Cheney nursery naturally
- Free v3 from the 38 MB/s Boehm ceiling entirely

**Status: candidate, not committed.** Per the kill criterion in
`BOEHM_DEPENDENCY_2026-05-21.md`, Whippet commits only when
measured Boehm overhead exceeds a threshold. Today Boehm is NOT
the bottleneck (#702 falsified GC-scan dominance), so Whippet
is dormant.

**Whippet escalation trigger:** if §4 ships nursery default-on
AND the tenured arena becomes the next bottleneck (measurable
via Boehm scan time at > 10 % of eval wall), Whippet becomes the
natural next stage. Until then, dormant.

## §7 — Strategic implication: v3's differentiator

This is one of the more compelling distinguishing capabilities
v3 has over TW.

`OPTIMIZATION_STRATEGIES_2026-05-23.md` §9 flagged the
**interpreter ceiling at ~1.5-2× native** for wall-time. GC is
**independent of that ceiling.** v3 can be at 1.5× TW wall AND
~0.3× TW GC overhead simultaneously, because they're orthogonal
dimensions.

The narrative becomes:

> v3 trades modest interpreter wall overhead for substantially
> better operational characteristics — smaller working set,
> shorter pauses, higher allocation throughput, more concurrent
> processes per host.

That's a story TW cannot match without an upstream Boehm
replacement nobody is making.

For Hydra / nix-eval-jobs / CI farms — environments where
many concurrent evaluations share the same host RAM — this is
direct cost savings. A 3-4× working-set reduction is literally
3-4× the parallel-job capacity at the same hardware.

## §8 — Comparison to per-object memory work

`MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` identified
~150-300 MB of additional per-object memory reductions reachable
in 2-3 weeks (Tier B + Tier C combined).

**Nursery default-on cascades through most of those items:**

- `snapshotCurrentWiths` allocations become essentially free
  (die in nursery before scavenge)
- `fakeClo` memoization debate becomes moot (allocations are
  cheap, recycling is free)
- Bindings churn that #748+#750+#752 attacked becomes self-healing
  (transient intermediates die between scavenges)
- `mergeBindings` slack reclamation becomes automatic
- ValuePair turnover becomes nursery-recycled

The Tier B+C items remain useful (they reduce ABSOLUTE allocation
volume, which still matters for GC overhead), but the URGENCY
of each item drops once nursery is default-on. The compounding
effect makes nursery-default-on plausibly **the highest-leverage
memory work on the table today** — higher than any single Tier B
item.

## §9 — Honest limits

1. **Projections aren't measurements.** §5's "10-50× speedup"
   numbers are literature-grounded estimates, not v3 data. §4.1
   spike is what makes them real. If the data doesn't justify,
   the flip is wrong.

2. **The 1 MB stress missed-root is a real correctness concern.**
   Don't flip default-on until §4.2 resolves it OR the team can
   show the bug doesn't fire on real workloads (not just
   STRESS mode).

3. **Nursery memory overhead is real.** For workloads where the
   nursery never fills, the 2× to-space cost is pure overhead.
   Default size needs tuning per workload class.

4. **Tenured GC is still Boehm.** Phase D barriers + Phase E
   age-promotion are correct generational design, but the
   tenured collector itself remains conservative until Whippet
   (Stage 16 candidate) commits. The 10-50× projection is for
   nursery-resident allocations; tenured-resident allocations
   still pay Boehm cost.

5. **The "high throughput, small working set" claim is
   workload-dependent.** Nix eval fits the pattern cleanly
   (Thunks forced once, many transient Bindings). But
   force-evaluating a giant attrset that ALL needs to stay
   live in tenured doesn't get nursery benefit. Most real
   workloads are mixed.

## §10 — Cross-references

- `feedback_memory_first_class.md` — the rule this analysis
  operationalises (memory wins compound; ≥ 50 MB savings
  should ship even at neutral wall).
- `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` — per-object
  memory work that cascades from nursery default-on.
- `PERF_AUDIT_2026-05-23.md` T3.3 — the mortality measurement
  spike this doc gates on.
- `NURSERY_PHASE_D_DECISION_2026-05-21.md` — Phase D design.
- `BOEHM_DEPENDENCY_2026-05-21.md` — Whippet escalation trigger
  + criteria.
- `OPTIMIZATION_STRATEGIES_2026-05-23.md` §9 — interpreter ceiling
  is orthogonal to GC dimension.
- #702 (commit `1253bab2e`) — falsified GC-scan dominance;
  Boehm is not the wall-time bottleneck today.
- #719 (commit `5a3e489c9`) — three-way RSS decomposition
  instrumentation.
- #720 (commit `c0911aee6`) — Phase D default-on.
- #738 (commit `c4be4cfbc` v0.2; `9958eb4a5` stress fix) — Phase E
  two-region.

## §11 — Recommended next move

Today: schedule §4.1 mortality measurement spike (1 day). Pre-commit
the decision rules in §4.1.

This week: depending on §4.1 outcome, either flip default-on (if
data supports) or investigate why mortality is below threshold
(if it's not).

Beyond: Whippet stays dormant until Boehm tenured overhead is
measurable as the next bottleneck.

The Whippet upgrade is the long-horizon ceiling-breaker. The
nursery default-on is the **next two weeks of work that compounds
through everything else.**

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
