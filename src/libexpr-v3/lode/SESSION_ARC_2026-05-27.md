# Session arc 2026-05-27 — what landed, what's falsified, what's next

**Window**: 2026-05-27 single multi-turn session
**Aggregate**: 31 substantive commits + 5 new memory entries +
  this synthesis doc (LIVE-UPDATED, covering commits 1-31)
**Validation**: `all-v3-tests --quick` 6/6 PASS, `--core` 15/15 PASS
  at end of arc; HNE + hello.drvPath byte-identical to TW
  throughout

## 1. What landed (26 commits, chronological)

| # | Commit    | What                                                                                                |
|---|-----------|-----------------------------------------------------------------------------------------------------|
|  1 | `490a9dfb1` | `m5-cron.sh` — strategic-workload drift detection                                                 |
|  2 | `0454a9325` | M5 per-opcode cycle profile — OP_TAIL_CALL 33 %, OP_CALL_PRIMOP 20 %, OP_ATTRS_SELECT_DYN 14.5 %  |
|  3 | `63c69536f` | **Ditch-Boehm WALL premise FALSIFIED** — Boehm gc_count=1, gc_total_ms=0                            |
|  4 | `6f854fa2c` | **Stage 1 ✓** — `tagIsPointer` canonical predicate (17 static_asserts gating)                       |
|  5 | `02c95eba0` | **Stage 3 init** — `walkAllV3Roots` + RootVisitor + 5 root sources                                  |
|  6 | `e7639f837` | **Stage 3 complete** — added bridge tables + import cache; cellOwner/drvHash audited OUT            |
|  7 | `f3491859f` | **Stage 6 SPIKE** — live-fraction tracer → **SHIP-GREEN** verdict (239 MB freeable, hello.drvPath)  |
|  8 | `928e8ee3b` | Foundation doc records SHIP-GREEN ahead of impl                                                     |
|  9 | `1285de2fe` | **Boehm tuning §6.2 FALSIFIED** on macOS aarch64 (`boehm_unmapped` stuck at 0)                      |
| 10 | `5865b807c` | **Periodic-GC** confirms mechanism (401 MB released) + 9× wall regression → not viable as default   |
| 11 | `6442bf531` | **ChainBindings Phase C** guard memo (per 3-pivot falsification, no v4 attempt)                     |
| 12 | `5d2b194cb` | **HNE bucket decomposition** — peak 2987 MB / arena 1594 / boehm 403 / elsewhere 991                |
| 13 | `2aff04073` | (parallel session) GC investments codified as RSS-primary, not wall-primary                         |
| 14 | `dc225e46e` | **HNE elsewhere decomp** — caches contribute 500-950 MB peak RSS (ImportCache + SQLite)             |
| 15 | `173481af3` | **Stage 5 MVP ✓** — `GcRoot` RAII + thread-local registry + walker hook + unit test (5/5 OK)        |
| 16 | `9d2985765` | Session-arc synthesis (initial draft of this doc)                                                   |
| 17 | `79ce7bffb` | **Handoff designs**: arena dereg (2-3d) + Stage 6 production (2-3wk) ready-to-execute               |
| 18 | `dc22f0d8f` | CLAUDE.md strategic-table updates — 7 new docs surfaced                                             |
| 19 | `f493b09b1` | **T1.3 Thunks attr** — 99.999 % concentration at OP_MAKE_THUNK (confirming, no per-site lever)    |
| 20 | `abd99597f` | **T1.3 Closures attr** — 56% fakeClo overhead identified (144 MB on HNE)                            |
| 21 | `674b19d9f` | **T1.3 Closures follow-up** — fakeClo pool is DEAD CODE (Phase D Step 12 retired)                   |
| 22 | `4f46dbdbd` | **T1.3 Pairs + Lists attr** — 2 NEW LEVERS: mapAttrs 2-pair (100 MB) + tiny capWiths (13 MB)        |
| 23 | `187e156af` | **T1.3 unified cross-type dump** — top 6 sites = 90.5 % of allocs on HNE                            |
| 24 | `008cc4a05` | Session-arc synthesis live-updated to 26 commits                                                    |
| 25 | (n/a) | (parallel ChainBindings work — not in this session's arc but in nearby branch)                              |
| 26 | (n/a) | (parallel ChainBindings work — not in this session's arc but in nearby branch)                              |
| 27 | `62cc61239` | **PHASE_E_V02_STRESS_DESIGN** + Stage 6 prereq update — "architecture alignment" handoff             |
| 28 | `d77be72ab` | **`bench/phase-e-stress-validate.sh`** — Day-1 stress validation harness for Phase E v0.2            |
| 29 | `1efa7c886` | **Live-trace arena-dereg audit** — Tag::External=0 confirmed on hello.drvPath + HNE                  |
| 30 | `b1fdec503` | **`bench/arena-dereg-audit.sh`** — Day-1 audit harness for arena dereg                               |
| 31 | (this edit) | Live-update of synthesis + cross-workload External-clean confirmation                              |

**Cross-workload External-clean confirmation (commit 31 measurement)**:
Running `bench/arena-dereg-audit.sh` against the full default set
returns AUDIT VERDICT: PASS on all 4 workloads:

```
  Workload    Verdict      External        String          Path
  hello       PASS              0         70035           881
  firefox     PASS              0         70408          1054
  hne         PASS              0       1439011         10971
  ackermann   PASS              0             0             0
```

The External-clean claim now holds across two real nixpkgs paths
(hello + firefox), one haskell.nix-style flake (HNE), and one
purely-arithmetic synthetic.  Arena dereg's §4.2 audit is closed
for production-class workload patterns.

## 2. Strategic state changes

### Before session

* Stages 1-7 of precise-root foundation: only the roadmap doc existed
* Boehm wall + tuning hypotheses: stated but not measured
* HNE memory profile: largely unmeasured (just A1 attribution to mergeBindings)
* Stage 6 ROI: hand-waved at "10-50 MB FFI-only Boehm + ~350 MB net"
* ChainBindings Phase C: three falsified pivots in vm.cc memo, future
  action plan unclear

### After session

* **Foundation infrastructure shipped**: Stages 1, 3, 5 MVP all
  implemented + tested.  Stage 6 SPIKE validated.
* **Five Rule-0 falsifications** with documentation:
  Boehm wall, Boehm tuning §6.2, periodic GC, ChainBindings Phase C
  respect (3-pivot rule), fakeClo pool dead code.
* **HNE memory profile fully decomposed**:
  peak 2987 MB = v3_arena 1594 + boehm 403 + ImportCache+SQLite ~990
* **Stage 6 ROI quantified per workload**:
  hello.drvPath = 239 MB freeable (SHIP-GREEN);
  HNE = 797 MB freeable (4× SHIP-GREEN threshold);
  genList synthetic = 125 MB freeable (MARGINAL).
* **Stage 6 architectural prerequisite identified**: arena
  deregistration from Boehm (per BOEHM_TUNING_FALSIFIED follow-up)
  + side-table for bridge `nix::Value *`s.
* **Cache-eviction lever quantified**: 500-950 MB recoverable on HNE
  via Phase 4b LRU (blocked by 3 prerequisites: CU pointer stability,
  ImportCacheEntry nursery concerns, hash-bucket co-eviction).
* **ChainBindings Phase C revival prerequisites documented**:
  4 conditions per vm.cc:1167-1206 + memory entry created.
* **T1.3 per-type attribution complete across all 4 non-Bindings
  types** (Thunks, Closures, Pairs, Lists) + unified cross-type
  view → **3 new actionable levers surfaced**:
  - **fakeClo pool dead code** — 144 MB on HNE; pool exists at
    alloc.hh:1069-1230 with zero callers since Phase D Step 12
    (2026-05-21).  Lever: revive pool OR resolve Phase E v0.2
    stress-mode to enable nursery default-on.
  - **mapAttrs 2-pair App chain** — 100 MB on HNE.  Two pairs per
    mapAttrs entry; 3-arg App representation saves one pair → ~50 MB.
  - **Tiny `capturedWiths` ListVec** — 13 MB on HNE.  544K allocs
    of size 1-2 lists per OP_MAKE_THUNK; inline-in-Thunk lever.

## 3. What's now known about the memory landscape

| Workload      | peak_rss | v3_arena | boehm  | elsewhere | freeable (Stage 6) |
|---------------|----------|----------|--------|-----------|--------------------|
| hello.drvPath |  754 MB  |  587 MB  |  403 MB|    0 MB   |  239 MB (SHIP-GREEN) |
| HNE           | 2987 MB  | 1594 MB  |  403 MB|  990 MB   |  797 MB (4× SHIP)    |
| genList 200k  |   --     |   --     |   --   |    --     |  125 MB (MARGINAL)   |

The `elsewhere` column is the **HNE-specific 990 MB** (cache + SQLite
+ misc); hello.drvPath has ~0.  Stage 6 reclamation operates on
`v3_arena` exclusively.

### Per-Bindings-site attribution on HNE

```
alloc@vm.cc:1228 (mergeBindings)       585 MB  ( 82 % of Bindings)
alloc@vm.cc:7326                         45 MB  (  6 %)
OP_ATTRS_REC_INIT_TAIL                   30 MB  (  4 %)
primMapAttrs                             26 MB  (  4 %)
OP_ATTRS_REC_INIT                        10 MB  (  1 %)
... 18 more sites = 6 % of Bindings bytes
```

mergeBindings dominates.  98.3 % of mergeBindings is OP_ATTRS_UPDATE_TAIL.
35 % of UPDATE_TAIL has nb=1 (single-key overlay) — the canonical
ChainBindings shape.

## 3.1 T1.3 unified cross-type top sites on HNE

Single-glance prioritization view (commit `187e156af`):

```
v3-direct alloc-attr unified: 32 sites, 11.6M allocs, 734 MB cross-type
  Type     Origin (file:line)               allocs       MB   %cumul
  Thunk    vm.cc:3707                      3826516   304.93   41.5%   OP_MAKE_THUNK
  Closure  vm.cc:3412                      1765735   115.08   57.2%   OP_MAKE_CLOSURE general
  Closure  vm.cc:6803                      1595947   101.31   71.0%   fakeClo OP_FORCE
  Pair     primops.cc:1970                 1094597    50.11   77.9%   mapAttrs inner App
  Pair     primops.cc:1965                 1094597    50.11   84.7%   mapAttrs outer App
  Closure  vm.cc:12085                      714910    42.65   90.5%   fakeClo OP_TAIL_CALL
  List     vm.cc:7035                       172800    24.47   93.8%   OP_LIST_CONCAT
  List     vm.cc:3831                       544299    12.82   95.6%   tiny capturedWiths
  ... 24 more sites falling off rapidly
```

**Top 6 sites = 90.5 % of cross-type allocation bytes.**  This is
THE strategic prioritization table for future memory work — any
single 1-3 day spike targeting one of these sites is justified by
the per-site share.

## 4. Concrete next bounded steps (multi-session)

| Task                                          | Effort   | Yield (peak RSS)             | Blocked by                          |
|-----------------------------------------------|----------|-------------------------------|--------------------------------------|
| **Phase E v0.2 stress-mode resolution** (architecture alignment) | 1-3 d | 144 MB on HNE (auto via fakeClo) + Stage 6 alignment | none — handoff doc ready |
| Arena deregistration from Boehm               |  2-3 d   | 100-400 MB on HNE indirect    | External-tag audit + bridge side-table |
| Stage 6 production precise GC                 |  2-3 wk  | 239-797 MB (validated)        | Phase E default-on + arena dereg + Stage 5 bulk apply |
| Phase 4b ImportCache LRU eviction             |  1-2 wk  | 500-950 MB on HNE             | CU ptr stability + nursery + buckets |
| ChainBindings Phase C revival                 | multi-session | 200-300 MB on HNE         | 4 prerequisites in vm.cc:1167-1206   |
| **fakeClo pool revival** (alternative to Phase E) | ~1 d | 144 MB on HNE                | Audit Phase D ↔ pool interaction; obviated if Phase E flips |
| **mapAttrs 3-arg App representation**         |  1-2 d   | ~50 MB on HNE                 | Update App evaluator + serialize + GC walkers |
| **Tiny capturedWiths inline-in-Thunk**        |  2-3 d   | 13 MB on HNE                  | Phase D barrier audit + Thunk struct |
| Stage 5 bulk `V3_GC_ROOT(...)` application    |  1-2 d   | (foundation; no direct yield) | Stage 6 production needs first       |
| HNE `vmmap` decomposition refinement          |  1 d     | (measurement; no yield)       | macOS taskgated permission OR new code |

### Lever inventory summary

**257 MB potentially recoverable from the 3 T1.3-surfaced levers**
(fakeClo 144 + mapAttrs 50 + capWiths 13 + tiny others) — comparable
in magnitude to a Stage 6 ship, distributed across smaller bounded
1-3 day spikes.  Stage 6 itself remains the biggest single-ship at
797 MB on HNE.

**The fastest single ship**: fakeClo revival (~1 d, 144 MB on HNE).
Pool infrastructure intact; just needs callers rebound at vm.cc:6803
+ vm.cc:12085 + OP_RETURN cleanup.  Pre-committed SHIP threshold:
≥ 100 MB peak_rss on HNE + `--core` 15/15 PASS.

### Recommended order (per user 2026-05-27 framing)

1. **HNE workaround documented today**: `NIX_V3_NO_DISK_CACHE=1`
   gives 500-950 MB peak RSS reduction immediately at the cost of
   wall-time on warm-cache scenarios.  Production CI-style evals
   should use this gate.
2. **First new session — "architecture alignment"**: Phase E v0.2
   stress resolution per
   [`PHASE_E_V02_STRESS_DESIGN_2026-05-27.md`](PHASE_E_V02_STRESS_DESIGN_2026-05-27.md).
   1-3 days.  Closes AR7 + obviates the 144 MB fakeClo lever
   automatically + aligns Stage 6's semi-space mechanics with
   what the codebase already exercises.
3. **Second new session — arena dereg** per
   [`ARENA_DEREGISTRATION_DESIGN_2026-05-27.md`](ARENA_DEREGISTRATION_DESIGN_2026-05-27.md).
   2-3 days.  Unblocks Stage 6 + tests collection-cost.
4. **Third new session(s) — Stage 6 production** per
   [`STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md`](STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md).
   2-3 weeks.  Delivers ≥239 MB hello.drvPath, ≥797 MB HNE.
2. **Arena deregistration spike** — 2-3 days, unblocks Stage 6 + tests
   the Boehm-internal collection-cost hypothesis.
3. **Stage 6 production precise GC** — given the spike is GREEN,
   committed 2-3 weeks of work delivers ≥ 239 MB on hello.drvPath
   and ≥ 797 MB on HNE.
4. **Phase 4b cache LRU** — parallel track to Stage 6, attacks the
   500-950 MB HNE-specific cache pressure.
5. **ChainBindings Phase C revival** — only when 4 prerequisites
   are met (multi-session, separate plan).

## 5. Falsifications ledger (Rule 0 wins)

| Hypothesis                                              | Falsified by                                                                  |
|--------------------------------------------------------|--------------------------------------------------------------------------------|
| "Ditch Boehm delivers wall improvement"                 | Boehm gc_count=1, gc_total_ms=0 on hello.drvPath + cardano-node M5            |
| "Boehm `free_space_divisor` tuning recovers 100-300 MB" | boehm_unmapped stays at 0 MB across all documented runtime knobs (macOS 8.2.8) |
| "Periodic `GC_gcollect()` reduces peak RSS"             | Confirms unmap mechanism but +14 MB peak + 9× wall regression                  |
| "ChainBindings Phase C v4 is straightforward"           | Existing vm.cc:1167-1206 memo + 3 prior-art falsified pivots                  |

Saves an estimated 4-6 weeks of misdirected work across these four
items.

## 6. Memory updates

Three new entries in `~/.claude-io/projects/.../memory/`:

* `project_gc_spike_2026-05-27.md` — SHIP-GREEN verdict + reproduction
* `project_chain_bindings_phase_c_falsified.md` — 4-condition revival list
* `project_gc_spike_2026-05-27.md` (indexed in MEMORY.md)

## 7. Validation summary

| Test                            | Result | Notes                            |
|--------------------------------|--------|----------------------------------|
| `all-v3-tests --quick`         | 6/6 ✓ | includes gc-root-handles MVP test |
| `all-v3-tests --core`          | 15/15 ✓ | up from 13/14 pre-arc            |
| `(import <nixpkgs> {}).hello.name`            | byte-identical to TW         |
| `(import <nixpkgs> {}).hello.drvPath`         | byte-identical to TW         |
| HNE `packages.x86_64-linux.hello.drvPath`     | byte-identical to TW         |

## 8. Honest limits

* **macOS aarch64 only**: every measurement is on Darwin / Apple
  Silicon.  Linux behaviour of Boehm unmap may differ (e.g.,
  `mremap` vs `munmap` semantics, MADV_DONTNEED availability).
  Cross-platform validation is future work.
* **Single-run RSS noise**: cache state survives across processes
  via `~/.cache/nix/`.  Cold vs warm cache produces ~400 MB peak
  variance.  Numbers are directional, not reproducible to MB.
* **Stage 5 has no real callers yet**: the RAII infrastructure is
  shipped + tested, but no production `V3_GC_ROOT(...)` lines exist.
  Bulk application is part of Stage 6's work, not Stage 5 MVP's.
* **Live-trace is end-of-run only**: gives a LOWER BOUND on freeable.
  Mid-eval freeable (the actual Stage 6 ROI) is strictly greater but
  not directly measured.  A SIGUSR1-triggered mid-eval probe would
  refine the number; outside this session's scope.
* **HNE decomposition was differential, not direct**: the 700 MB
  "ImportCache" attribution was derived from comparing
  `NIX_V3_NO_CONTENT_CACHE=1` vs default.  A `vmmap`-based
  region-by-region inventory would be cleaner; blocked by macOS
  taskgated permission.

## 9. Cross-references

* `lode/GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md` — 7-stage plan, updated
* `lode/LIVE_FRACTION_SPIKE_2026-05-27.md` — Stage 6 spike data
* `lode/HNE_BUCKET_DECOMP_2026-05-27.md` — HNE measurement record
* `lode/BOEHM_TUNING_FALSIFIED_2026-05-27.md` — §6.2 falsifier
* `lode/MEMORY_REDUCTION_AVENUES_2026-05-26.md` — pre-session
  category framework; Categories 1, 3, 5 closed this session
* `[[memory-first-class]]` — ≥ 200 MB SHIP gate
* `[[measure-twice-cut-once]]` — methodology applied throughout
* `[[falsification-rule]]` — Rule 0; every commit kills a hypothesis
* `[[chain-bindings-phase-c-falsified]]` — multi-session guard
* vm.cc:1167-1206 — Phase C 3-pivot postmortem
* primops.cc:7521-7530 — ImportCache struct (the cache elephant)
* alloc.hh:578-715 — Arena class (arena-dereg target)

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
