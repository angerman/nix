# ROADMAP alignment — post-#741 arc, 2026-05-23

After the full #741 IFD content-addressed eval-result cache
architectural arc landed (11 commits, 11 Rule-0 falsifiers, full
cross-process persistence + 95% cross-workload reuse + 0 wall
savings on hello.drvPath), this doc captures the ROADMAP state and
next-up priorities.

Companion to `ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md` (yesterday's
state) and `ALIGNMENT_NOTE_2026-05-23.md` (mid-session alignment).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.

---

## Stage state

```
Stage 1  Action plan          ~90%       █████████████████░  IN PROGRESS
Stage 2  Pure-bytecode eval   ✅ CLOSED   ████████████████████ CLOSED POSITIVE
Stage 3  Nursery default-on   ~95%       ███████████████████ ESSENTIALLY DONE
Stage 4  Uniform STG-shape    ~50-60%    ██████████░░░░░░░░  v4.3/4.4 landed; #776 pending
Stage 5  Hidden classes       ✗ KILLED   [cancelled]         2.24% < 10% threshold
Stage 6  Polymorphic ICs      ✗ KILLED   [cancelled]         (implicit, built on S5)
Stage 7  Selector thunks      ~10-15%   ██░░░░░░░░░░░░░░░░  needs post-S5/6 redesign
Stage 8  Thin FFI / parallel  ~50-55%   ███████████░░░░░░░  #741 substrate complete
Stage 9  Module linking       ✗ KILLED   [cancelled]         1.17× < 2× threshold
─────────── UX pillars post-perf ─────────────────────────────────
Stage 14 Error UX             ~10%       ██░░░░░░░░░░░░░░░░
Stage 15 Profiler UX          ~35-40%   ███████░░░░░░░░░░░
Stage 17 Pattern lint UX      ~5%        █░░░░░░░░░░░░░░░░░
```

## What's been mapped this session

The #741 IFD cache architecture is **FULLY MAPPED**:

| Aspect | Outcome |
|---|---|
| Round-trip serialiser | ✓ correct, 56× margin under target |
| Cross-process canonical hash | ✓ deterministic, 0 cross-process diff |
| In-memory drv-hash cache | ✓ 33% hit rate matches output-dup prediction |
| Cross-process persistence (SQLite) | ✓ 100% warm hit + 95% cross-workload reuse |
| Active skip-on-hit | ✓ byte-identical TW preserved |
| **In-process wall savings** | ✗ 0 ms net (1.00× ± 0.02) |
| **Cross-process wall savings** | ✗ +71% cold / +5.6% warm |
| Batched SQLite inserts (Phase 5b) | ✗ +98% cold, variance 24× |
| forceDeep-at-primop-entry (Phase 3a-RCA-A) | ✗ shapeCell pollution |
| forceDeepReadOnly (Phase 3c-RCA-B) | ✗ same shapeCell RCA |

The cache machinery is production-quality (gated, all default-off,
correctness-validated).  The wall economics question is **closed on
hello-scale workloads**: no in-architecture caching strategy moves
the needle, because the libstore-tail target (~30-50 µs / call) is
too small relative to cache overhead.

## Strategic conclusions about the ROADMAP

Three findings reshape the ROADMAP:

1. **The V8-perf path is closed** (Stages 5/6/9 all killed
   2026-05-22/23 by Rule 0).  The actual realised path is:
   memory + caching + mechanical-tier + implement-then-revert
   (per session-arc 2026-05-23).

2. **#741 in-process caching is closed**: 11 falsifiers
   conducted, architecture fully mapped, wall lever too small.
   Cache machinery retained as gated infrastructure; #741 work
   pivots to either deeper-graph workloads or Phase 4 (Class B
   IFD primops where build cost is seconds).

3. **The next high-leverage step is unknown**: with Stages 5/6/9
   killed and #741 in-process closed, the obvious-next-lever
   structure is exhausted.  Concrete options below; selection
   needs measurement spike.

## Next-session priority list (post-#741-arc)

Priority-ordered by leverage + scope:

### Tier 1 — Untested workloads (where #741 might still pay off)

  1. **Workload diversification on cardano-node M5**.  Test whether
     deeper derivation graphs have larger per-call libstore tail.
     Falsifier: COLD ACTIVE+DISK ≤+10% OR WARM ≤noise on cardano-node M5.
     Scope: 1-2 sessions (depends on M5 wall variance).

  2. **Phase 4 — Class B IFD primops**.  Hook `OP_IFD_PROBE` →
     `disk_cache::lookupEvalResult` for `import`/`readFile`/
     `pathExists` on derivation outputs.  Cached value: the
     bridged result.  Falsifier: haskell.nix hello-world warm
     eval ≥30% faster.  Scope: 2-3 sessions; needs haskell.nix
     workload setup.

### Tier 2 — Concrete optimisation candidates

  3. **#776 Let-floating** (Stage 4 v4 followup).  Move non-strict
     let bindings to use site to avoid premature thunk allocation.
     Falsifier: hello.drvPath wall improvement ≥3%.  Scope: 1-2
     sessions.

  4. **#660 cleanup**: delete primV3ForceAttr / primV3CallBridge1 /
     primV3ForceListElem TW-side bridges.  Mechanical refactor,
     LOC reduction.  Scope: ~1 session.

### Tier 3 — Larger architectural

  5. **Stage 7 redesign**: Selector thunks were designed to build
     on Stage 5 shapes (killed).  Redesign without shapes — e.g.
     pattern-recognised at lower time via `inherit (a) ...`
     detection.  Scope: 1 session design + 2-3 implementation.

  6. **Eval-level content-addressed memoization** (Unison-style):
     attack the 33%-intra-process-duplicate finding at root.
     LARGE architectural shift; needs full design proposal first.
     Scope: months.

  7. **Whippet GC or other moving-GC investigation**: enables
     compaction; opens Stage 5 shapes via stable shape interning.
     But Stage 5 was killed by 2.24% dispatch share, so even with
     stable shapes the lever is bounded.  Scope: months; deferred.

### Tier 4 — Pure information-gathering

  8. **Workload heterogeneity audit**: measure wall profile across
     hello, gcc, python3, firefox, nixos-toplevel, cardano-node,
     haskell.nix.  Identify per-workload hot levers.  Scope: 1
     session of measurement.

## Recommended order

The CHEAPEST falsifier-first ordering:

  Tier 4 #8 (1 day) → identifies workloads where the wall structure
  differs from hello.drvPath
    → if a workload has bigger libstore-tail cost: do Tier 1 #1
       (validate #741 there)
    → if a workload has different bottleneck: re-strategise
    → if all workloads look like hello.drvPath: do Tier 2 #3
       (#776 let-floating) or Tier 2 #4 (#660 cleanup)

This is a one-session-cost spike that maximises information yield
for next-session direction selection.

Alternative: just commit to Tier 1 #2 (Phase 4 Class B IFD primops)
— it's the original S4 scope per IFD_DEEP_DIVE and has the largest
expected wall savings (seconds per IFD).  But needs haskell.nix
workload setup which is its own session.

## Operating rules codified this session

  * **Implement-then-revert is valid Rule 0**: #741 produced 5
    falsifiers (forceDeep, forceDeepReadOnly, Phase 3e ACTIVE wall,
    Phase 5 wall, Phase 5b batching) — each consumed 30-60 min of
    work + measurement, each resolved a specific architectural
    question.  Pattern: implement carefully, measure rigorously,
    commit either positive landing OR revert-with-data.

  * **Gated infrastructure is OK after falsified hypothesis**: the
    cache machinery (Phases 1-5) is RETAINED even though wall
    savings are zero.  Each gate is default-off; cost when off is
    ~0.  Future re-investigation (cardano-node, Phase 4) can
    re-use the substrate.

  * **Architectural mapping has value beyond wall**: the 33%
    intra-process duplicate rate and 95% cross-workload reuse are
    SIGNIFICANT informational findings even though we couldn't
    monetise them on hello-scale workloads.  They constrain
    future design.

## Cross-references

  * `[[741-arc-complete-2026-05-23]]` — #741 arc memory
  * `lode/IFD_CACHE_DESIGN_2026-05-23.md` — 5-phase plan
  * `lode/ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md`
  * `lode/ALIGNMENT_NOTE_2026-05-23.md`
  * `lode/SESSION_ARC_2026-05-23.md` — methodology arc
  * `lode/IFD_DEEP_DIVE_2026-05-21.md` — S4 strategy source
  * `lode/CELL_UPDATE_EVERYWHERE_2026-05-12.md:169` — Phase 3a/3c
    RCA precedent
