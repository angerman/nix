# Session plan — Bindings polymorphism + Stage 4 strictness + Stage 3 nursery

**Date**: 2026-05-20  
**Context**: User asked to "finish the rest of all the steps necessary to clear Bindings
polymorphism, Stage 4 strictness, and Stage 3 nursery."  Each is multi-week per
ROADMAP_TO_VISION_2026-05-15.md.  This document lists the realistic execution units
and tracks status across this and future sessions.

## Honest scope statement

This session lands the MVP / first-measurement for each track and a detailed plan for
the remainder.  Full completion of all three is multiple sessions of work.

## Tracking

| # | Task | Phase | Status |
|---|------|-------|--------|
| (a) | Bindings polymorphism MVP — Empty/Single sentinel | A | in_progress |
| (b) | Cardano-node bytes-breakdown measurement | A | pending |
| 703a | Bindings Small shape (≤4 entries inline) | B | pending |
| 703b | Bindings Sorted shape audit (no-op verify) | B | pending |
| 703c | Bindings caller migration (vm.cc, primops.cc, lower.cc, etc.) | C | pending |
| 703d | Bindings polymorphism re-measure on hello.drvPath + cardano-node | C | pending |
| 704a | Stage 4 strictness — analysis-pass scaffold | A | pending |
| 704b | Stage 4 strictness — strict-position emit elision in lower.cc | B | pending |
| 704c | Stage 4 strictness — measurement on hello.drvPath thunks-allocated | C | pending |
| 705a | Stage 3 nursery — Phase D audit instrumentation | A | pending |
| 705b | Stage 3 nursery — Phase D write barrier MVP | B | pending |
| 705c | Stage 3 nursery — default-on regression sweep | C | pending |

Phases:
- **A** — MVP / first measurement / kill a hypothesis (lands this session if possible)
- **B** — meaningful implementation (1-3 days each)
- **C** — completion / regression safety / default-on (1+ week each)

## Order of execution this session

1. (a) Bindings Empty/Single sentinel + re-measure
2. (b) Cardano-node measurement
3. 704a Stage 4 scaffold (analysis pass identification)
4. 705a Stage 3 audit instrumentation
5. Write multi-session execution plan for B/C phases

## Rule 0 anchors per task

| Task | Hypothesis to kill |
|------|--------------------|
| (a) | "Empty/Single sentinel cuts ≥ N% of Bindings bytes" — quantify N |
| (b) | "cardano-node has the same Bindings-dominant profile as hello.drvPath" |
| 704a | "Strict positions are statically identifiable without dataflow" — yes/no |
| 705a | "Inter-generational pointer rate is below threshold for write-barrier viability" |

## Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
