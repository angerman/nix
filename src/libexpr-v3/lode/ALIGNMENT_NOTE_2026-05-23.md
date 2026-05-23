# Roadmap alignment — 2026-05-23 post-#792 / pre-#741

A short alignment note linking today's mega-session work to the
strategic doc set + the post-kill roadmap state.  Companion to
`SESSION_ARC_2026-05-23.md` (the methodology arc) and
`IFD_CACHE_DESIGN_2026-05-23.md` (the next architectural lever).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.

---

## 1. Roadmap state after the three kills

Three stages have been killed by Rule 0 in the last 2 days, all
with pre-committed thresholds and measured data:

| Stage | Original scope | Kill threshold | Measured | Calendar reclaimed |
|---|---|---|---|---|
| Stage 5 | Hidden classes / attrset shapes | AttrSelect ≥ 10% dispatch | 2.24% on hello.drvPath | ~6 weeks |
| Stage 6 | Polymorphic ICs | Built on Stage 5 | (implicit) | ~6 weeks |
| Stage 9 | Module linking / bytecode dedup | ≥ 2× function-level collapse | 1.17× on two workloads | ~5 weeks |
| **Total** | | | | **~17 weeks calendar reclaimed** |

What remains active:

```
Stage 1  Action plan         ~80-90% done
Stage 2  Pure-bytecode eval  ✅ CLOSED 2026-05-22 (13 weeks early)
Stage 3  Nursery default-on  ~95% done
Stage 4  Uniform STG-shape   ~40-50% (v4.3 cross-fn strictness done; v4.4 next)
Stage 7  Selector thunks     ~10-15% (3 weeks remaining)
Stage 8  Thin FFI / parallel ~35-45% (disk-cache; #741 lives here)
─────────── UX pillars post-perf ─────────────────────────────────
Stage 14 Error UX            ~10%
Stage 15 Profiler UX         ~35-40%
Stage 17 Pattern lint UX     ~5%
```

## 2. Where today's session work lands

Today's commits (chronological):

1. `14c8da998` → `9c9eea6e2` → `eed91b3dd` — the 9-commit rigor
   subsession.  Methodology corrections (RETRACTION blocks on #780/#783),
   measurement infrastructure (OPCYCLES + per-primop wall-clock),
   two implement-then-revert cycles (#785 OP_SET_LOCAL_KEEP, #791
   __derivCoerce shortcut), and the #792 audit.

2. `cd7b9c7af` — `#741 design doc` (this commit, today).

This is **NOT** Stage-1-through-7 advancement work.  It's
**Stage 8 measurement infrastructure + design preamble**:

- Stage 8 is "thin FFI / parallel + disk-cache productionisation."
- The measurement infrastructure (#786, #788, #790) is the
  instrument layer needed to scope Stage 8 levers.
- The #785/#791 ship-or-revert cycles validated the
  implement-then-revert discipline that closes Stage-8-style
  small-lever investigations.
- The #792 audit ruled out mechanical-redundancy levers in
  derivation primops, focusing the remaining work on the only
  architectural lever (#741).
- The #741 design doc is the **engineering pre-work for Stage 8's
  remaining capacity-lever**.

## 3. Next-up priorities (post-this-session)

Ordered by current best-estimate Rule-0 leverage:

| # | Task | Estimated effort | Why it's next |
|---|---|---|---|
| 1 | **#741 Phase 1** | 3-5 days | Validates the architecture before committing to the multi-week feature.  Falsifier: round-trip serialiser + < 100 µs/result.  If it fails, kills #741 cheaply. |
| 2 | **Stage 4 v4.4** (cross-fn strictness through higher-order callees) | 1-2 weeks | The strictness substrate IS land-and-iterate; v4.3 plumbing is done; v4.4 makes it fire on more sites.  Direct wall savings on hello.drvPath via reduced thunk allocation. |
| 3 | **Workload diversification** | 1-2 days | All current measurement is hello.drvPath + cardano-node M5.  NixOS, gccCross, ghc-N attribute paths likely surface different hot levers.  1-day spike, high information yield. |
| 4 | **#741 Phase 2+3** | 1-1.5 weeks | Only after Phase 1 validates. |
| 5 | **#776 let-floating** | 4-5 days | Pending followup to #775; smaller dispatch-residue lever. |

The #741-then-Stage-4-v4.4 alternation matches the calendar of the
ROADMAP_PROGRESS_SNAPSHOT_2026-05-23 (Stage 4 v4.4 was scoped at
"next week candidate" — now actually next session candidate).

## 4. Strategic deltas to surface

Three observations worth carrying into the next session:

### 4.1 The "perf wall" is shaped differently than the 2026-05-15 ROADMAP assumed

The original ROADMAP_TO_VISION assumed wall-clock perf would come
from V8-style hidden-classes + PICs (Stages 5/6).  Both killed.
What replaced them:

- **Memory levers** (#748, #750, #752): peak_rss 4.3 GB → 1.08 GB
  on hello.drvPath in ~3 days, while wall ratio moved 30× → 1.45×.
- **Caching levers**: per-file disk cache (#770/#771), and now
  proposed eval-result cache (#741).
- **Mechanical levers**: small wall wins at sub-10% scale (#779
  OP_GET_REC_SLOT -24%, #777 deserializeCU -119 ms, #752 PosIdx
  inlined).
- **Methodology levers**: measurement-then-falsify discipline
  shipping clean reverts (#785 +2.8 ms, #791 -4.8 ms) when
  the lever is sub-threshold.

The "V8 perf path" being closed is OK because the actual perf
path was different.  Memory + caching + mechanical-tier-attribution
+ implement-then-revert is the realised strategy.

### 4.2 Stage 8 is doing more work than originally scoped

Per the original roadmap, Stage 8 is "thin FFI + parallel infra
(W9-W44)."  In practice it has absorbed:

- The disk-cache substrate (#770/#771 default-on; #777
  deserializeCU optimisation; #781 sub-instrumentation).
- The IFD cache (#741, this session's design).
- Class-A derivation-construction caching (#741 extension).
- The implement-then-revert measurement infrastructure.

Stage 8 is effectively "the perf-stage-8 + cache-stage" hybrid.
Worth noting if the ROADMAP gets a renumbering pass: the
"thin FFI + parallel infra" name no longer captures the work.

### 4.3 The "warm-eval is the user-facing target" reframe

Per ROADMAP §"Warm-eval is the primary user-facing target", v3's
caching wins are most valuable in warm-eval (i.e. CI / Hydra /
nix-eval-jobs) scenarios.  #741 is exactly the warm-eval lever
that materialises this framing:

- Cold eval: cache hashes + serialises + inserts (overhead).
- Warm eval: cache lookups skip ~80% of primop wall (huge win).

#741 belongs in the same category as the disk cache (#770/#771)
in this framing — both are user-facing-target work.

## 5. What stays the same

Despite the three stage kills, the broader strategic shape is
unchanged:

- **Rule 0 (every commit kills a hypothesis)**: vindicated.  The
  killed stages saved ~17 weeks via pre-committed thresholds.
- **V3-NATIVE constraint**: unchanged; #741's cache is pure-v3,
  no TW round-trip.
- **Memory-first-class** + **warm-eval-first** rules: both
  codified 2026-05-23; both apply to #741.
- **Implement-then-revert discipline**: validated 2× this session
  (#785, #791); now operating rule per SESSION_ARC.

## 6. Cross-references

- `ROADMAP_TO_VISION_2026-05-15.md` — original strategic plan;
  Stages 5/6 marked KILLED in-doc, Stage 9 in `STAGE_9_KILLED_2026-05-22.md`.
- `ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md` — yesterday's snapshot
  with the timeline projection (`end-state plausibly shifts left
  by 3-4 months`).
- `STAGE_5_6_KILLED_2026-05-23.md` — the #778 kill memo.
- `STAGE_9_KILLED_2026-05-22.md` — the #772 kill memo.
- `SESSION_ARC_2026-05-23.md` — the methodology arc summary.
- `IFD_CACHE_DESIGN_2026-05-23.md` — the #741 engineering design.
- `memory/project_strategic_docs_2026-05-15.md` — the 4-doc index.
