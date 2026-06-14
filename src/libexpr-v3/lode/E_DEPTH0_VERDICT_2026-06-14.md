# Workstream E (depth>0 GC + span reuse) — verdict (2026-06-14)

Goal: complete workstream E (the RSS structural backstop — fire GC mid-eval at
depth>0 so freed dead lines are recycled by subsequent allocation, capping the
high-water of the 224 MB scattered dead on firefox). **Verdict: E as specified
(NON-GENERATIONAL depth>0 GC) is FALSIFIED at its own stage-1 CPU kill bar.
Reviving E requires generational marking first (the nursery / Phase D write
barriers — unresolved).** Resolved by the plan's cheap-projection kill bar, with
NO risky depth>0 implementation (the plan: "E could die at stage 0/1 — by design,
for one day's cost").

## Stage 0 (done earlier): VIABLE on dead-line %
firefox mid-eval `L_resident` peak 0.438 (<0.80) + 42.7 % dead lines (>20 %) —
there IS reclaimable scattered dead. So the *opportunity* exists; the question is
whether reclaiming it fits the CPU budget. It does not.

## Stage 1 CPU kill bar: HIT (the decisive measurement)
The plan's stage-1 kill bar is "projected GC CPU >+15 % at a 64 MB window → E
dies." Measured on firefox.drvPath (laptop, min user-CPU over 3):

| arm | CPU | per-GC |
|---|---|---|
| GC-off (NIX_V3_NO_MAJOR_GC=1) | **3.38 s** | — |
| GC-on (one fire @256 MB) | 4.69 s | markMs=1936 + sweepMs=340 = **1310 ms** |

**ONE full-mark GC = +1310 ms = +38.8 %** — 2.6× the +15 % kill bar, from a SINGLE
fire. Capping the high-water needs ≥1 mid-eval fire (realistically several, since
firefox's cumulative allocation exceeds its peak), so the true cost is +38.8 % × N.
The bar is blown by the minimum.

## Root cause + why no cheaper path exists
- The mark is **CACHE-BOUND** (§1.8: ~225 ns/cell, 100 % precise-walk over the
  ~262 MB live set; the per-edge block lookup is <10 %). Block-aligned mmap can't
  fix it (§1.8 FALSIFIED). So each mid-eval GC pays a full ~1.3 s mark.
- The mark is **NON-GENERATIONAL**: it walks the WHOLE live set every fire, not
  just the cells allocated since the last GC. Mid-eval GC therefore re-marks
  hundreds of MB repeatedly.
- Span reuse (stage 2, the Immix line-recycling allocator at alloc.hh:1267-1307)
  is sound and EXISTS — but it only pays if the GC fires MID-eval (so freed lines
  are reused by later allocation). The single end-of-eval GC gives no reuse
  benefit. So E's value is entirely gated on cheap mid-eval GC, which the
  cache-bound non-generational mark forbids.

## The actual prerequisite
**Generational marking** — mark only the YOUNG set (cells allocated since the last
GC, found via a remembered set / write barrier) so each mid-eval GC is cheap.
That is the **nursery + Phase D write barriers**, which are UNRESOLVED (CLAUDE.md
constraint 0: "Phase D (write barriers) is unresolved"; default-OFF). Without it,
mid-eval GC cannot fit the CPU budget on firefox-class workloads.

**So "complete workstream E" reduces to "complete Phase D (the generational
nursery) first."** E's non-generational form is dead; its span-reuse machinery is
ready to compose with a generational collector once Phase D lands. This is a
multi-week prerequisite, not a same-session completion — and per Rule 0 the
honest move is to record the kill here rather than ship a non-generational depth>0
GC that blows the CPU budget (and carries the H-B use-after-free hazard the
exitDepth==0 gate currently prevents).

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
