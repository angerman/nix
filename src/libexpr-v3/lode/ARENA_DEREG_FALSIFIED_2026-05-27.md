# Arena deregistration — STANDALONE HYPOTHESIS FALSIFIED

**Date**: 2026-05-27 evening
**Per**: `ARENA_DEREGISTRATION_DESIGN_2026-05-27.md` §6 "Expected
outcome" + §5 pre-committed SHIP gate
**Outcome**: ✗ **Standalone arena dereg delivers 0-1 MB RSS savings**
on both hello.drvPath and HNE — far below the FALSIFY threshold
(< 50 MB).  The design's hypothesis "arena dereg → Boehm collects
more often → smaller watermark" is empirically false.  But the
INFRASTRUCTURE landed remains valuable for Stage 6.

## Measurement

| Workload      | Baseline RSS | NIX_V3_ARENA_NOROOT=1 RSS | Δ      | gc_count |
|---------------|--------------|---------------------------|--------|----------|
| hello.drvPath |   753.0 MB   |          753.2 MB         |  +0.2 MB|  1 → 1    |
| HNE           |  2565.1 MB   |         2566.5 MB         |  +1.4 MB|  1 → 1    |

Per the design's pre-committed SHIP gate:
* SHIP: ≥ 100 MB peak RSS reduction on HNE
* TUNE: 50-100 MB
* **FALSIFY: < 50 MB OR any crash**

Observed: +1.4 MB (regression within noise).  **FALSIFY.**

## Why the design hypothesis was wrong

The design predicted (per §6):
> Today: gc_count=1, gc_total_ms=0, peak_rss = N MB
> After dereg: gc_count = 10-100 (auto-fired by Boehm's normal
> heuristic, now affordable), gc_total_ms ≤ 100 ms, peak_rss = N − Δ
> where Δ ≈ 200 MB on HNE

This conflates two mechanisms:

1. **Boehm per-collect COST**: yes, arena dereg drops this from
   ~40 ms (587 MB scan) to ~1 ms (registry-only scan).
2. **Boehm collect FREQUENCY**: NOT determined by per-collect
   cost.  Boehm fires when its OWN heap grows past a threshold
   (`free_space_divisor` heuristic).

The MISSING LINK: v3 doesn't allocate from Boehm's heap.  All v3
cells live in the arena (malloc-backed, separate from Boehm).
Boehm's heap only holds TW-side allocations (libexpr's Bindings
during parse, etc.) which are small + one-shot at startup.

So Boehm's heap reaches its initial watermark (~400 MB at startup
from libexpr/parser init), then grows only marginally during the
v3 eval phase, never crosses the auto-collect threshold, never
triggers another collection.  Arena dereg makes each (rare)
collection cheaper but doesn't increase the frequency.

This was empirically verified earlier in the session arc via
`BOEHM_TUNING_FALSIFIED_2026-05-27.md`'s periodic-GC probe:
forced collections DID drop the watermark (401 MB → 1.4 MB) but
required +9× wall regression.  The auto-policy was already known
to not trigger.

## What lands regardless (NOT carcass)

The infrastructure is reusable for Stage 6:

1. **`include/v3/bridge_root_registry.hh` + `bridge_root_registry.cc`**:
   * Thread-local `std::vector<void *>` for `Thunk::bridgeSrc`
     values
   * Boehm-rooted via `GC_add_roots` on growth
   * Always-on (called from `Alloc::allocBridgeThunk`); zero
     overhead when arena is also Boehm-rooted (redundant but
     safe duplicate scan path)
   * Stage 6 precise GC will use this to know which `nix::Value *`
     references survive a major scavenge.

2. **`NIX_V3_ARENA_NOROOT=1` gate in `Arena::refill` + huge-block
   path** (`alloc.hh`):
   * Opt-in deregistration; default OFF preserves status quo
   * Tested PASS on hello.drvPath + HNE (byte-identical to TW)
   * Tested PASS on arena-dereg-audit (Tag::External=0 confirmed)

Both are KEPT — neither is "carcass behind gate":
* `bridge_root_registry` is a real concrete data structure with
  a real concrete caller (every `allocBridgeThunk`).
* `NIX_V3_ARENA_NOROOT` is the future Stage 6 hook: when Stage 6
  ships, it can rely on this gate to flip arena dereg from
  opt-in to default.

## What's killed

* "Standalone arena dereg delivers RSS savings on hello/HNE":
  FALSIFIED.
* "Boehm auto-collect frequency responds to per-collect cost":
  FALSIFIED — Boehm uses heap-growth pressure, not cost.

## What this means for the 144 MB fakeClo lever

The fakeClo overhead (144 MB on HNE per `T1_3_CLOSURES_ATTR`) is
in the v3 arena, not in Boehm's heap.  Arena dereg doesn't
reclaim arena bytes — it just stops Boehm from scanning them.
The lever still requires Stage 6 production GC (which reclaims
arena bytes by mark+copy) to close.

## What remains for Stage 6

Stage 6 production GC (per `STAGE_6_PRECISE_GC_DESIGN_2026-05-27`
+ `STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27`) is unchanged:
* Day 1 dual-region Arena MVP
* Days 2-5 Week 1 foundation
* Weeks 2-3 tuning + ship

The bridge_root_registry built here is a NEW prerequisite
satisfied — Stage 6's MoveGCVisitor doesn't need to special-case
bridge pointers via Boehm registration; the side-table already
does it.

## Cross-references

* `lode/ARENA_DEREGISTRATION_DESIGN_2026-05-27.md` — original
  design (now falsified standalone; infrastructure still useful)
* `lode/BOEHM_TUNING_FALSIFIED_2026-05-27.md` — sibling
  falsification that should have warned about Boehm's
  growth-not-cost decision heuristic
* `lode/STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md` — where the
  infrastructure gets reused
* `lode/STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md` — Day 1
  starts with dual-region Arena (compatible with this work)
* `[[falsification-rule]]` — every commit kills a hypothesis;
  this kills "standalone arena dereg"
* `[[measure-twice-cut-once]]` — pre-committed thresholds;
  measurement says FALSIFY

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
