# Integrated v3 GC strategy — post-architectural-pin discovery

**Date:** 2026-05-30
**Author:** session synthesis (3-agent deep design + critical cross-review + direct code verification)
**Status:** STRATEGIC DESIGN — supersedes individual GC variant designs; layered architecture across 6 tiers
**Triggering question:** "Design a proper GC strategy carefully. Break down the GC architecture into distinct tasks/steps."

Companion docs (load-bearing):
- [`BRIDGES_HOLD_RETENTION_2026-05-29.md`](BRIDGES_HOLD_RETENTION_2026-05-29.md) — bridges hold 99.8% of live arena
- [`WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md`](WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md) — Path A page-release blocker (THE PIN)
- [`PHASE_4B_LRU_FALSIFIED_2026-05-30.md`](PHASE_4B_LRU_FALSIFIED_2026-05-30.md) — cache eviction falsified
- [`EXIT_DAY2_CACHE_PROXY_FALSIFIED_2026-05-30.md`](EXIT_DAY2_CACHE_PROXY_FALSIFIED_2026-05-30.md) — `NIX_V3_NO_DISK_CACHE=1` proxy falsified
- [`DIAGNOSTIC_AUDIT_2026-05-29.md`](DIAGNOSTIC_AUDIT_2026-05-29.md) — diagnostic gap analysis
- [`L_TIME_SERIES_DATA_2026-05-29.md`](L_TIME_SERIES_DATA_2026-05-29.md) — L(t) measurement
- [`STAGE_6_CHENEY_FALSIFIED_2026-05-27.md`](STAGE_6_CHENEY_FALSIFIED_2026-05-27.md), [`PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`](PHASE_4_PRELIM_FALSIFIED_2026-05-29.md) — GC variant falsifications
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) — pre-commit threshold methodology
- [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) — predecessor plan (this supersedes)

---

## 1. TL;DR — the single architectural finding

**The v3 arena is an append-only mmap allocator with NO page-release mechanism (no `madvise(MADV_DONTNEED)`, no `munmap` of partial blocks). Verified by code inspection of `include/v3/alloc.hh`: zero matches for `madvise`, `MADV_DONTNEED`, `munmap`.**

This single architectural fact explains **nine** falsified tracks:
1. Ditch-Boehm wall premise (was wrong about wall; arena is the bucket)
2. Boehm tuning §6.2 (Boehm doesn't unmap; v3 arena doesn't use Boehm)
3. Periodic GC (forced collection doesn't unmap arena)
4. Standalone arena dereg (broke Boehm root chain; arena pages stay)
5. Cheney semispace (copies live; OLD arena pages stay until destructor)
6. Flat mark-sweep (cells freed to free-list-bins; arena pages stay)
7. **Weak bridge eviction (Stages 0/1/1.5/2/2b LANDED, 0 MB ROI)** — references dropped, pages stay
8. **Phase 4b LRU cache eviction (LANDED, +177 MB regression on HNE)** — entries dropped, pages stay
9. **`NIX_V3_NO_DISK_CACHE=1` proxy (+457 MB regression on HNE)** — even disabling cache shows the same pin

**The proper strategy is not "pick a new GC variant." It is a layered architecture where the bottom layer (arena page-release) is fixed first; everything else cascades.**

The six layers, in dependency order:

| # | Layer | What it does | Effort | Depends on |
|---|---|---|---|---|
| 1 | **Arena page-release** | madvise/munmap on freed regions | 1 wk spike + 1-2 wk impl | nothing |
| 2 | **Precise stack scan** | Replace conservative C-stack scan with GcRoot RAII | 1-2 wk | nothing (parallel with L1) |
| 3 | **Bridge cohort lifecycle** | Mechanism 1b: cohort weak refs with purity bit | 1-2d spike + 3-5d impl | L1 |
| 4 | **Cache eviction (re-evaluate)** | Possibly demoted; current Phase 4b LRU was falsified | TBD | L1 + measurement |
| 5 | **Per-site allocation fixes** | Continue Tag::App3 / mergeBindings / per-site bundles | ongoing | nothing (parallel) |
| 6 | **GC variant choice** | Flat MS rescue OR Immix completion OR new design | TBD | L1+L2+L3 + measurement |

**Pre-commit acceptance for the entire strategy:** by Day 30, ship Layers 1+2+3 with M5 peak RSS ≤ 4 GB watchdog. If after L1 ships, the bridge-release cascade alone gets M5 under watchdog, the strategy succeeded; L4+L6 become deferred polish.

---

## 2. The architectural pin — code-verified evidence

### 2.1 Direct code check

```bash
$ grep -n "madvise\|MADV_DONTNEED\|munmap" include/v3/alloc.hh
(no matches)
```

Arena allocation path (`alloc.hh:1043-1090`): `mmap` to acquire blocks, bump-pointer alloc within blocks, never returns pages. `freeWholeBlock` exists (`alloc.hh:1692-1745`) but it merely removes the block from `active_.blocks` — the underlying mmap'd pages remain mapped via the original mapping.

### 2.2 Empirical evidence — five independent measurements

Per `PHASE_4_PRELIM_FALSIFIED §2.3`:
- HNE under MS gate-ON: v3_arena delta −84 MB, elsewhere delta +45 MB → net peak −39 MB (within 130 MB 2σ noise)
- The −84 MB arena reduction did NOT translate to RSS reduction

Per `WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md`:
- Stage 2 serialize-on-evict measured 0 MB peak_rss reduction
- Logical references dropped, arena pages stayed mapped

Per `PHASE_4B_LRU_FALSIFIED_2026-05-30.md`:
- LRU at MAX_ENTRIES=50: +177 MB regression on HNE
- Cache entries dropped, replacement allocations grew arena, no page release

Per `EXIT_DAY2_CACHE_PROXY_FALSIFIED_2026-05-30.md`:
- `NIX_V3_NO_DISK_CACHE=1` proxy: +457 MB regression on HNE (cold cache rebuild grew arena)

Per `DIAGNOSTIC_AUDIT_2026-05-29.md` §2.1 + DIAG-1:
- `blocksFreed=0` every cycle on HNE (no whole-block-free fires)
- Free-list-bin resident bytes accumulate

**Five independent measurements; one architectural pin.** This is the strongest signal in the entire 3-week arc.

### 2.3 Why DIAG-1 measured `blocksFreed=0`

Per Agent B verification of `mark_sweep.cc:766-772`:
```cpp
fullyDead = (blockLiveCells == 0 && !marker.anyMarkInRange(...))
```

Two conditions: zero live cells AND no interior marks (Tag::Slot interior pointers into Bindings::entries[i] kept the container alive).

DIAG-1's `blocksFreed=0` is from **cause (a) cells too dense**, not bookkeeping bug. With bridges holding ~519 MB live on HNE scattered across all ~100 blocks, every block has SOME live cell. No block hits the fully-dead predicate.

**The strict predicate is correct.** What needs to change is either:
- (i) Live-set concentration (so blocks DO go fully dead) — Layer 3 (bridge release)
- (ii) Compaction (move scattered live cells to free up blocks) — major refactor, deferred
- (iii) **Page release within partially-live blocks via madvise on dead spans** — Layer 1

Layer 1 + Layer 3 together would make whole-block-free fire AND madvise dead spans within still-occupied blocks. Combined, they should produce real RSS reduction.

---

## 3. The 9 falsifications and the common-mode root cause

| # | Track | Falsified | Root cause |
|---|---|---|---|
| 1 | Ditch-Boehm wall | `63c69536f` | Boehm wall=0; bucket misidentified |
| 2 | Boehm tuning §6.2 | `1285de2fe` | Boehm doesn't unmap; arena doesn't use it |
| 3 | Periodic GC | `5865b807c` | Forced collection doesn't unmap arena |
| 4 | Standalone arena dereg | `bad371821` | Broke Boehm chain; pages stay (PIN) |
| 5 | Cheney semispace | `eda44711a` | 2× peak intrinsic; OLD pages stay (PIN) |
| 6 | Flat MS Phase 4 | `PHASE_4_PRELIM_FALSIFIED` | free-list-bins resident; pages stay (PIN) |
| 7 | Weak bridge | `WEAK_BRIDGE_PAGE_RELEASE` | logical drop; pages stay (PIN) |
| 8 | Phase 4b LRU | `PHASE_4B_LRU_FALSIFIED` | entries evicted; pages stay (PIN) |
| 9 | `NO_DISK_CACHE` proxy | `EXIT_DAY2_CACHE_PROXY_FALSIFIED` | cache off, re-fill grows arena (PIN) |

**Pattern:** every falsification in the last 3 weeks has hit the same architectural pin. Per [[measure-twice-cut-once]] §3.7 (three failed pivots on same premise = falsification of the premise), **the meta-premise "v3 RSS is fixable without page-release semantics" is falsified.**

This drives the strategy: Layer 1 (page release) is non-optional and must ship first.

---

## 4. The 6-layer strategy

### Architecture diagram

```
┌─────────────────────────────────────────────────────────────────┐
│ LAYER 6: GC variant choice (DEFER until L1+L2+L3 ship)          │
│ Decision: flat MS rescued? Immix completed? New design?         │
│ Pre-commit: re-measure post-L1 arena; choose with data           │
├─────────────────────────────────────────────────────────────────┤
│ LAYER 5: Per-site allocation fixes (PARALLEL)                   │
│ Continue Tag::App3 / mergeBindings / capWiths pattern            │
│ Independent of L1-L4; Week 1 bundle pattern                     │
├─────────────────────────────────────────────────────────────────┤
│ LAYER 4: Cache eviction (RE-EVALUATE post-L1)                   │
│ Phase 4b LRU FALSIFIED; may be obsoleted by L1 cascade           │
│ Pre-commit: re-measure cache contribution AFTER L1 ships         │
├─────────────────────────────────────────────────────────────────┤
│ LAYER 3: Bridge cohort lifecycle (Mechanism 1b)                 │
│ Cohort weak refs + purity-bit re-eval; HNE 519 MB target        │
│ DEPENDS ON L1: requires page release to translate to RSS         │
├─────────────────────────────────────────────────────────────────┤
│ LAYER 2: Precise stack scan replacement                         │
│ Replace conservative C-stack scan with GcRoot RAII              │
│ INDEPENDENT of L1; parallelizable                                │
├─────────────────────────────────────────────────────────────────┤
│ LAYER 1: Arena page-release (THE PIN)                           │
│ madvise(MADV_DONTNEED) on freed regions; munmap whole blocks     │
│ MUST SHIP FIRST; everything downstream depends                   │
└─────────────────────────────────────────────────────────────────┘
```

### Layer 1: Arena page-release

**The single most important deliverable.** Without it, Layers 3-4 don't move RSS.

#### Mechanism

Two complementary techniques:

**1a — Whole-block munmap.** When `freeWholeBlock` fires (post-`mark_sweep.cc:899-932`'s 2nd-pass loop), actually unmap the mmap region instead of just removing from `active_.blocks`. Block size is 16 MB; per HNE post-bridge-release, ~80-100 blocks could become fully dead.

**1b — Span-level `madvise(MADV_DONTNEED)`.** Within partially-live blocks, identify contiguous dead spans (using the cellStarts/lineMarks bitmaps from existing Stage 6 infrastructure) and `madvise` them. The OS reclaims those pages immediately; future re-touch incurs a page fault (zero-fill) but no peak penalty.

**1c — Page-aligned dead-span detection.** A 16 MB block divided into 4096-byte pages = 4096 pages. If a 4 KB-aligned span of dead cells spans an entire page, madvise that page. Coalesce adjacent dead-span pages.

#### Tasks

| Task | Effort | Acceptance |
|---|---|---|
| 1.1 **`madvise` spike on synthetic workload** | 2 days | Manually unmap a known-dead region in a controlled test; measure RSS via `vmmap`/`smaps`; confirm page-release works on macOS aarch64 AND linux x86_64 |
| 1.2 **`freeWholeBlock` → munmap** | 2-3 days | When 100%-dead block detected, `munmap(blkStart, kBlockSize)`. Remove from `active_.blocks` AND release virtual address space. |
| 1.3 **Span-level madvise during sweep** | 3-5 days | After `sweepOneBlock` populates dead-cell list, identify page-aligned dead spans (≥4 KB contiguous dead, page-aligned), madvise them. |
| 1.4 **`bridge_root_registry` cleanup** | 1 day | After munmap, the bridge_root_registry entries pointing into unmapped blocks are stale. Sweep and clean. |
| 1.5 **Cross-platform validation** | 2 days | Test on macOS + linux; document any platform-specific behavior. |
| 1.6 **Measurement + SHIP gate** | 2 days | Run on hello.drvPath + HNE + M5; verify peak RSS reduction. |

**Total Layer 1 effort: 11-15 days.**

#### Pre-commit thresholds (per measure-twice §3)

Spike (Task 1.1) acceptance:
- Synthetic workload: allocate 100 MB, free 100 MB, madvise, measure RSS via vmmap
- **SHIP**: post-madvise RSS drops by ≥80% of freed bytes
- **FALSIFY**: <30% drop → mechanism doesn't work on this platform; investigate alternative (mmap reset? new mmap?)

Production SHIP gate (Task 1.6):
- hello.drvPath: peak RSS reduction ≥150 MB (target: 753 → ≤600)
- HNE: peak RSS reduction ≥300 MB (target: 2987 → ≤2700)
- M5: peak RSS reduction ≥500 MB (target: 5760 → ≤5260)
- Wall regression ≤10% on hello, ≤15% on HNE/M5
- 6/6 quick + 15/15 core tests pass
- byte-identical outputs across all workloads

Falsification criteria:
- If hello reduction <50 MB → page-release mechanism not working as expected
- If wall regression >25% → madvise/munmap is too expensive per cycle; need batching

#### Risks

| Risk | Mitigation |
|---|---|
| `madvise(MADV_DONTNEED)` semantics differ between macOS and linux | Cross-platform validation in 1.5; document divergence |
| Re-touch of madvised pages causes major page fault → wall regression | Coalesce dead spans; only madvise if ≥16 KB contiguous |
| `bridge_root_registry` entries dangle into unmapped blocks | Sweep registry in 1.4; per-entry validate-or-drop |
| Whole-block `munmap` interacts badly with the GC walker (stale pointers in worklist) | Walker must consult `inActive` before deref; verified at `mark_sweep.cc:215+` |
| Boehm's conservative scan crosses unmapped regions → SIGSEGV | Boehm scan range is `(arenaMin, arenaMax)`; unmapping reduces range, doesn't violate. Validate. |

### Layer 2: Precise stack scan replacement

**Independent of Layer 1; can run in parallel.**

Per Agent B: the conservative C-stack scan (`mark_sweep.cc:594-672`) walks ~1M pointer-sized words on Apple Silicon's 8 MB pthread stack per GC, marks any that fall in arena range, then transitively walks each marked cell. **At mid-eval peak, this retains thousands of cells from dispatch-loop locals, primop body locals, and valueStack `Value*` references that aren't truly live.**

#### Mechanism

Replace `walkCStackConservative` with comprehensive GcRoot RAII at hot dispatch sites. Stage 5 (`173481af3` Day-1 MVP) shipped the infrastructure; needs comprehensive wiring.

#### Tasks

| Task | Effort | Acceptance |
|---|---|---|
| 2.1 **GcRoot audit** | 2 days | Enumerate all hot dispatch sites + primop bodies that hold arena pointers as locals. Cite count and locations. |
| 2.2 **Wire GcRoot at OP_FORCE / OP_CALL / primop entry** | 3-4 days | RAII registration; benchmark per-op overhead |
| 2.3 **Conservative scan reduction** | 2-3 days | Gate conservative scan behind `NIX_V3_CONSERVATIVE_STACK=1` (default OFF post-wiring). Replace with precise registry walk. |
| 2.4 **Stress mode validation** | 2 days | `NIX_V3_MAJOR_GC_STRESS=1000` + brute audit; verify no missed roots |

**Total Layer 2 effort: 9-11 days.**

#### Pre-commit thresholds

- Per Agent B: at mid-eval peak, conservatively-marked cell count should drop from "thousands" to <5% of precise live-set
- Wall regression from RAII registration ≤5% on hello
- 6/6 + 15/15 + brute audit PASS

Falsification:
- If conservative-marked count doesn't drop substantially → the dispatch-loop locals are genuine roots (eval semantics dependency); the conservative scan is correct; revisit at allocator level instead

### Layer 3: Bridge cohort lifecycle (Mechanism 1b)

**DEPENDS ON LAYER 1.** Without page-release, dropping bridge references is memory-inert (proven by Stage 2 weak-bridge work).

Per Agent A: the recommended mechanism is **cohort-scoped fallbackExpr re-eval with purity bit**, NOT per-bridge fallback (Stage 1.5 falsified for per-bridge capture coverage).

#### Mechanism

Each bridge entry tagged with its source CU + root-Value slot. CU classified as PURE or IMPURE at compile time (no fetchTree / readFile / impure primops). On memory pressure or cohort-end trigger, drop the v3Value of all bridges in a PURE cohort. On re-access, re-run the CU from its root-Value slot.

#### Tasks

| Task | Effort | Acceptance |
|---|---|---|
| 3.1 **Purity classifier spike** | 1-2 days | Static AST walk; classify each CU as PURE/IMPURE. Measure % of bridge bytes attributable to PURE cohorts on HNE/M5. |
| 3.2 **Cohort table + per-bridge cohort tagging** | 2-3 days | Add `cohortCU` + `cohortRootValueSlot` + `cohortFlags` to bridge entries (+16 B/entry; M5: 160 KB) |
| 3.3 **Cohort sweep policy** | 2 days | Trigger: L-trough detection (per DIAG-3 L(t) data) OR per-K-MB-alloc safepoint |
| 3.4 **Re-eval path** | 2-3 days | `replayCohort(cohort, rootIdx)` re-runs CU; replace Stage 2's blob-deser path |
| 3.5 **Differential stress test** | 2 days | `NIX_V3_BRIDGE_COHORTS_STRESS=1` evicting every cohort; nixpkgs byte-identity across 5 anchor packages |

**Total Layer 3 effort: 9-12 days. Cannot start until Layer 1 SHIPS.**

#### Pre-commit thresholds

Stage A purity spike (Task 3.1):
- **CONTINUE**: PURE-CU coverage ≥30% of bridge bytes on HNE
- **STOP**: <30% — re-eval-based eviction structurally underpowered; pivot to allocator-level work

SHIP gate (after Layer 1 ships):
- HNE peak RSS reduction ≥400 MB attributable to bridge cohort release (additional to Layer 1's contribution)
- M5 peak RSS reduction ≥800 MB attributable
- Re-eval correctness: 0 byte-identity mismatches under stress
- Wall regression from re-eval ≤10% on hello/HNE

#### Risks (per Agent A's risk register, code-verified)

| Risk | Mitigation |
|---|---|
| Re-eval non-determinism (Stage 1.5 falsification: HNE produced divergent drvPath after replay) | Cohort PURITY bit defaults FALSE for CUs that called `installBytecodePrimop` or captured external upvalues. Stress test validates. |
| TW caches handle-keyed pointers across drop+re-create | Handle int is stable (index never moves; entries evict-marked not removed). Audit `tryUnwrapBridge1Closure` (already done; safe per Agent A). |
| Cycle-detection regression | Cohort-replay depth bound + per-cohort in-flight set |
| Boehm finalizer concurrency | Lock-free cohort sweep; defer multi-thread story |

### Layer 4: Cache eviction (RE-EVALUATE post-L1)

**Phase 4b LRU has been FALSIFIED.** Current Phase 4b LRU code is correctness-clean but memory-inert (per `PHASE_4B_LRU_FALSIFIED_2026-05-30.md`).

**However:** the falsification happened in a regime where the arena couldn't release pages. **Post-Layer-1, cache eviction may behave differently** — evicted entries free arena cells whose pages can then be madvise'd.

#### Tasks

| Task | Effort | Acceptance |
|---|---|---|
| 4.1 **Re-run Phase 4b LRU under Layer 1** | 2 days | After Layer 1 ships, re-measure LRU at MAX_ENTRIES=50/100/200 on HNE |
| 4.2 **Decision: ship or shelve** | 0 days | Binary verdict per pre-commit |

**Total Layer 4 effort: 2 days IF Layer 1 cascades to make eviction effective. Indefinite shelf otherwise.**

#### Pre-commit thresholds

Re-measurement (Task 4.1):
- **SHIP**: ≥150 MB peak RSS reduction on HNE under combined Layer 1 + Phase 4b LRU
- **SHELVE**: <50 MB additional reduction beyond Layer 1 alone

#### What's not in this layer

- SQLite tuning (Agent C identified as untried; ≤1d spike, possibly +30 MB)
- ImportCache `cus` deque eviction (requires CU pointer-stability work, ~2-4 weeks; do NOT start per Agent C — arena re-allocation regression risk)
- AOT cache integration (already done; not a memory lever)

### Layer 5: Per-site allocation fixes (PARALLEL, ongoing)

Continue Week 1 SHIPS pattern: T1.3 per-Tag attribution → identify hot site → per-site fix → bundle measurement.

Already-known candidates (post-Week-1):
- mergeBindings over-allocation (Phase C pattern, 4-prereq plan needed)
- Additional T1.3 follow-ups
- Tag::App3 extensions

Independent of L1-L4; can run in parallel.

#### Pre-commit threshold

Per per-site fix:
- ≥30 MB on HNE OR ≥50 MB on M5 to ship
- Wall-neutral or wall-positive (no wall regression)

### Layer 6: GC variant choice (DEFER post-L1+L2+L3)

After Layers 1-3 ship, **re-measure arena behavior on hello/HNE/M5.** The post-cascade heap profile may be very different:
- L might drop dramatically (bridges released → less live)
- Whole-block-free might fire frequently (cascade from L1)
- Conservative-marked cells might collapse (L2)

With NEW measurements:
- **Flat MS rescued?** If quadratic fix (`clearCellStartBitFor` O(blocks × deadCells)) + L1 + L2 makes it ship, the Immix detour is unnecessary.
- **Immix needed?** If even post-cascade, partial-block density remains an issue, Immix's mark-region wins.
- **New design?** Possibly something between (sticky mark-bits, segregated free lists, etc.)

**Pre-commit:** do NOT pick a GC variant before L1+L2+L3 ship. Decision is purely measurement-driven.

#### Tasks

| Task | Effort | Acceptance |
|---|---|---|
| 6.1 **Post-L1+L2+L3 measurement** | 2 days | Hello/HNE/M5 under combined ON; full DIAG suite output |
| 6.2 **Variant decision** | 1 day | Pick variant based on measurements; document rationale |
| 6.3 **Variant implementation** | TBD | Effort depends on choice |

---

## 5. Sequencing and dependencies

```
Week 1-2:  Layer 1 (madvise spike + impl)        ◄── CRITICAL PATH
           Layer 2 (precise stack scan, parallel) ◄── parallel
           Layer 5 (per-site fixes, parallel)     ◄── parallel
                       ↓
Week 3:    Layer 1 SHIP gate measurement
           Decision: Layer 1 passes → Layer 3 begins
                     Layer 1 fails  → re-spike; do NOT begin L3
                       ↓
Week 4-5:  Layer 3 (bridge cohort)
           Layer 4 (re-run Phase 4b LRU)
                       ↓
Week 6:    Combined measurement (L1+L2+L3+L4+L5)
           M5 watchdog test
                       ↓
Week 7+:   Layer 6 (GC variant decision based on new measurements)
```

**Total to M5-watchdog SHIP: 6 weeks if all layers pass their SHIP gates.**

### Critical-path identification

**Layer 1 is the critical path.** If Layer 1 fails its SHIP gate:
- Layer 3 doesn't ship (no RSS impact)
- Layer 4 stays shelved
- Layer 6 has no improved data to decide on
- Net: weeks of work that don't move M5 RSS

Therefore: **all engineering energy until Layer 1 SHIPS or FALSIFIES goes into Layer 1.** Layer 2 and Layer 5 can run in parallel only insofar as engineering capacity exists; if singletrack, Layer 1 takes precedence.

### Falsification cascade

If Layer 1 falsifies (post-spike, page-release doesn't translate to RSS):
- The architectural pin is something OTHER than page-release (perhaps Boehm scan range, or process-allocator metadata)
- Investigate `vmmap` / `pmap` to find what's actually holding the pages
- Possibly: rebuild on top of Boehm's own arena (give up the per-thread bump allocator entirely)

This is a 3-month strategic pivot; would require re-architecting v3's allocation story. Should be VERY rare given the convergent evidence.

---

## 6. Critical review of agent findings

### Agent A (Bridge lifecycle)

**Strong:**
- Code-verified existence of `fallbackExpr` field per BridgeXEntry
- Identified Stage 1.5 falsification (per-bridge fallback divergent drvPath on HNE)
- Recommended Mechanism 1b (cohort weak refs) — empirically-grounded
- Risk register caught the Page A page-release blocker

**Where I push back:**
- Mechanism 1b's "cohort end trigger" relies on L-trough detection. Per Agent B, mid-eval L variation is driven by VMState frames + conservative scan as much as by bridges. Trigger may fire unreliably.
- The per-CU purity classifier is sketched but not detailed. Edge cases (callPackage chains, lib.fix recursion) may classify IMPURE in ways that drop coverage below 30%.
- Agent A treats the page-release blocker as a parallel concern; I'm promoting it to Layer 1 with strict gating.

### Agent B (Post-bridge-release arena)

**Strong:**
- Code-verified the `clearCellStartBitFor` quadratic (`alloc.hh:1750-1765`)
- Identified conservative C-stack scan as the silent root multiplier
- Walked the 6 falsifications and classified each post-bridge-release status
- Quantified: mark:sweep = 7:1, not 40-54%

**Where I push back:**
- Agent B's "conservative scan is the load-bearing problem" claim is strong but n=1 (one analysis, not multiple measurements). Layer 2 needs to verify the conservative-marked cell fraction at mid-eval peak BEFORE committing to comprehensive GcRoot wiring.
- The "VMState frames retain the active force chain" argument is true but doesn't fully explain why DIAG-1 measured blocksFreed=0 — bridges hold MORE than just the active force chain.
- The flat-MS-rescue analysis assumes Layer 1 ships; if not, the quadratic fix alone won't matter.

### Agent C (Cache eviction)

**Strong:**
- Code-verified Phase 4b LRU exists and is gated
- Documented the THREE falsified cache tracks (Phase 4b LRU, NO_DISK_CACHE proxy, original HNE_BUCKET_DECOMP estimates)
- M5 elsewhere ≈ 0 MB correction (I had estimated ~3 GB; wrong)
- SQLite tuning analysis: ≤1d spike possible

**Where I push back:**
- Agent C demotes cache eviction entirely; I'm keeping it as Layer 4 RE-EVALUATE because post-Layer-1 the regime may be different (evicted entries' arena cells now actually free pages via madvise).
- The "3 prereqs were red herrings except (a)" is sharp but doesn't address whether (a) CU pointer stability becomes solvable under Layer 1's page-release semantics.
- Cache contribution may have been overstated historically (700 MB claim → 568 MB actual) but it's still 500+ MB of arena that's NOT touched by GC. Worth re-evaluating post-L1.

### Synthesis-level critical observations

**Two things the agents didn't address:**

1. **The strategy's own falsifiability.** What kills "fix the pin first" as a meta-strategy? If Layer 1 ships and we get <50 MB on HNE, the pin wasn't the pin. That's a real risk. Mitigation: Task 1.1's spike with binary acceptance criteria BEFORE committing to 1-2 weeks of implementation.

2. **The interaction between layers.** Each agent analyzed its layer in isolation. Are there second-order effects?
   - Layer 1 madvise + Layer 3 bridge release: bridges hold pointers into arena; if we madvise those pages before bridges drop, dangling. **Order matters: drop bridges first, then GC, then madvise.**
   - Layer 2 precise scan + Layer 1: precise scan may need to know about madvised regions (skip them in walk). Walker must consult `inActive` AND not deref into unmapped pages.
   - Layer 5 per-site fixes + Layer 1: per-site fixes that prevent allocation entirely (Tag::App3) reduce arena growth; combined with L1, peak drops more.

These interactions are not blockers but require integration testing in Week 6.

---

## 7. Pre-committed acceptance for the entire strategy

**By Day 30 (post-Week 6 combined measurement):**

| Workload | Current | Target | Watchdog |
|---|---|---|---|
| hello.drvPath peak RSS | 753 MB | ≤ 400 MB (-45%) | n/a |
| HNE peak RSS | 2987 MB | ≤ 1800 MB (-40%) | n/a |
| **M5 peak RSS** | **5760 MB** | **≤ 4096 MB** | **4 GB watchdog** |

**Headline criterion: M5 under 4 GB watchdog.** If this ships, the strategy succeeded.

**Per-layer SHIP attribution** (cumulative):
- Layer 1 alone: hello ≤600, HNE ≤2700, M5 ≤5260 (target -500 MB minimum)
- Layer 1 + 3: hello ≤500, HNE ≤2300, M5 ≤4500
- Layer 1 + 2 + 3: hello ≤450, HNE ≤2000, M5 ≤4200
- Layer 1 + 2 + 3 + 4 + 5: hello ≤400, HNE ≤1800, M5 ≤4000

**Falsification path:** if by Week 3 Layer 1 fails its SHIP gate (madvise doesn't translate to RSS), the entire strategy needs a fundamental re-think — possibly rebuilding v3's arena on top of Boehm's allocator entirely. This is a 3-month strategic pivot.

---

## 8. Effort summary

| Layer | Effort | Calendar weeks | Critical path |
|---|---|---|---|
| 1 — Arena page-release | 11-15 d | Week 1-2 | ✓ CRITICAL |
| 2 — Precise stack scan | 9-11 d | Week 1-3 (parallel) | parallel |
| 3 — Bridge cohort | 9-12 d | Week 4-5 (after L1) | sequential after L1 |
| 4 — Cache eviction re-eval | 2 d | Week 5 | sequential after L1 |
| 5 — Per-site fixes | ongoing | parallel throughout | parallel |
| 6 — GC variant choice | TBD | Week 7+ | sequential after L1+L2+L3 |
| **Combined measurement** | 3 d | Week 6 | sequential |
| **M5 watchdog validation** | 2 d | Week 6-7 | sequential |

**Total to M5 watchdog SHIP: 6 weeks if all layers pass.**

**Single-engineer estimate.** Parallelism with 2 engineers: could compress to 4-5 weeks (Layer 2 + Layer 5 run truly in parallel during Layer 1).

---

## 9. Honest limits

- **The page-release mechanism is platform-sensitive.** macOS `madvise(MADV_DONTNEED)` semantics differ from Linux. Task 1.5 validates cross-platform; if behavior is incompatible, fallback strategies needed.
- **Conservative-stack-scan removal may break correctness.** Some C-stack pointers ARE genuine roots (active dispatch frame's Value*). Task 2.1 audit must catch these; failure mode is missed-root SIGSEGV under stress.
- **Bridge cohort purity may not generalize.** HNE's 3-entries-hold-519-MB pattern is concentrated; M5's 10K-dispersed pattern may not benefit from cohort granularity. M5-specific design may be needed.
- **The 6-week estimate is aggressive.** Each layer has its own risks; serial sequencing means a single slipped layer cascades.
- **Layer 1 SHIP gate (≥150 MB hello) is calibrated against AGENT B's analysis** that bridges + dead arena combine post-release. If conservative scan retention dominates (Agent B's "long pole"), Layer 1 alone may underperform; Layer 2 becomes mandatory not optional.
- **The strategy assumes the 9 falsifications generalize correctly to "all of them are the same pin."** If 1-2 of them were misclassified, the actual landscape may have additional independent obstacles. Per measure-twice §5.7, this is the moment-vs-distribution risk applied to falsifications themselves.
- **Determinism risk in Layer 3.** Re-eval of a cohort that captured shared state may produce different byte output. Task 3.5 stress test is the gate.
- **M5 baseline drift.** The 5760 MB is from `CARDANO_NODE_M5_2026-05-26`; today's number may differ ±10%. Per [[same-host-bisect]], re-measure before declaring M5 SHIP.
- **Daemon/REPL/IDE mode out of scope.** Process-exit assumption fails for long-running v3 processes; for those, Layer 4 (cache TTL) becomes more important.
- **The strategy says nothing about wall.** Wall regressions are tracked at per-layer SHIP gates but no aggregate wall target. v3 currently at ~1.4× TW on hello / ~3× on M5; layers shouldn't regress this materially.
- **No measurement plan for non-arena buckets** (Boehm 403 MB, libc malloc residue, syscall overhead). If non-arena bucket grows during the cascade, the picture changes; need vmmap probe per layer.

---

## 10. What this strategy is NOT

- **NOT a new GC variant.** No mark-sweep / mark-compact / Immix / Cheney / etc. proposed. Layer 6 defers that decision.
- **NOT a comprehensive memory-system overhaul.** The arena allocator structure stays; only the page-release semantics change at Layer 1.
- **NOT an architectural rewrite.** Stage 3 walker / Stage 5 GcRoot / bridge_root_registry / mark_sweep.cc all stay; layered additions only.
- **NOT a guarantee.** Pre-commit thresholds at every layer allow falsification. By design, the strategy can die at any layer without ambiguity about "should we have shipped it."

---

## 11. Cross-references

### Strategic context
- [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) — predecessor plan; this supersedes
- [`DIAGNOSTIC_AUDIT_2026-05-29.md`](DIAGNOSTIC_AUDIT_2026-05-29.md) — diagnostic infrastructure (now mostly built)
- [`SESSION_END_SYNTHESIS_2026-05-29.md`](SESSION_END_SYNTHESIS_2026-05-29.md) — bridge finding context
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §3 + §5.7 — pre-commit + methodology audit

### Layer-specific docs
- Layer 1: [`WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md`](WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md) — Path A analysis
- Layer 2: Stage 5 GcRoot landing commit `173481af3`
- Layer 3: [`BRIDGES_HOLD_RETENTION_2026-05-29.md`](BRIDGES_HOLD_RETENTION_2026-05-29.md), Agent A's Mechanism 1b sketch
- Layer 4: [`PHASE_4B_LRU_FALSIFIED_2026-05-30.md`](PHASE_4B_LRU_FALSIFIED_2026-05-30.md), [`EXIT_DAY2_CACHE_PROXY_FALSIFIED_2026-05-30.md`](EXIT_DAY2_CACHE_PROXY_FALSIFIED_2026-05-30.md)
- Layer 5: T1.3 trio + Tag::App3 + Week 1 SHIPS
- Layer 6: re-measure post-L1; no design yet

### Falsification register (9 total)
1-3: `GC_DITCH_BOEHM_FALSIFIED_2026-05-27.md`, `BOEHM_TUNING_FALSIFIED_2026-05-27.md`, periodic-GC commits
4: `ARENA_DEREG_FALSIFIED_2026-05-27.md`
5: `STAGE_6_CHENEY_FALSIFIED_2026-05-27.md`
6: `PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`
7: `WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md`
8: `PHASE_4B_LRU_FALSIFIED_2026-05-30.md`
9: `EXIT_DAY2_CACHE_PROXY_FALSIFIED_2026-05-30.md`

### Code anchors
- Architectural pin: `include/v3/alloc.hh` (no madvise/munmap/MADV_DONTNEED)
- Whole-block-free: `alloc.hh:1692-1745`
- Sweep predicate: `mark_sweep.cc:766-772`
- Conservative scan: `mark_sweep.cc:594-672`
- Bridge tables: `primops.cc:3930-3989` + accessors `3950-3956, 4211-4226`
- Phase 4b LRU: `primops.cc:8121-8181`
- L(t) periodic: `live_trace.cc:1343-1558`
- GcRoot RAII: `include/v3/gc_root.hh`
- `bridge_root_registry`: `include/v3/bridge_root_registry.hh`

### Methodology
- [[falsification-rule]] — every layer must answer "what hypothesis does this kill"
- [[memory-first-class]] — RSS-primary framing throughout
- [[measure-twice-cut-once]] §3 — pre-commit thresholds at every layer
- [[same-host-bisect]] — measure-then-compare before claiming regressions
- [[threshold-recalibration-rule]] §3.8 — recalibration only via premise correction

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
