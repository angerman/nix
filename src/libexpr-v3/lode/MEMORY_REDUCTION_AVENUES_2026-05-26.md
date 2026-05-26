# Memory reduction avenues — what's left after the recent falsifications

**Date:** 2026-05-26 (evening)
**Author:** session synthesis
**Status:** active — actionable avenue inventory post-RSS-stagnation observation
**Triggering question:** "What are our avenues left for memory reduction?" after observing that the team has been working on memory infrastructure for 48+ hours with 0 RSS reduction shipped.

Companion docs:
- [`MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md`](MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md) — original audit (pre-A1a falsifications); this doc operationalises remaining items
- [`HNE_MEMORY_ATTRIBUTION_2026-05-26.md`](HNE_MEMORY_ATTRIBUTION_2026-05-26.md) — A1 measurement source
- [`DIRECTION_NOTE_2026-05-26.md`](DIRECTION_NOTE_2026-05-26.md) — the RSS-stagnation observation that triggered this doc
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) — current tactical plan
- [`ARCHITECTURE_CRITIQUE_2026-05-26.md`](ARCHITECTURE_CRITIQUE_2026-05-26.md) — broader architectural view; T1.2 / T1.3 / T3.2 from PROFILING_IMPROVEMENTS feed in here

---

## 1. Position (TL;DR)

**The team has tried 5 memory approaches in 48 hours; 0 shipped RSS reduction.** A1b (pointer-keyed merge cache), A1a Phase C v1, v2, v3 (mergeBindings → Chain conversion), and B2 (nursery default-on on hello/firefox) all FALSIFIED. The structural lever the team initially hypothesised isn't working at this representation level on HNE.

**The territory is not exhausted.** Seven categories of remaining work, with substantial measurement gaps that haven't been closed first. The most likely yield is in **per-site attacks on HNE specifically** (mirror of #748/#750/#752 playbook), **per-alloc-site instrumentation for non-Bindings types** (T1.3 — never done), and **Boehm tuning** (the heap is 99.9 % FREE on hello.drvPath per #702 — substantial tunable headroom).

**The big strategic move: measurement-first this week, implementation second.** Three Phase C falsifications + A1b falsification + B2 falsification jointly suggest the team's mental model of HNE's allocation patterns may be wrong; more implementation without more measurement is unlikely to land. Five measurement spikes (~4-5 days total) would surface what HNE's actual patterns are.

**The honest strategic alternative:** if Week 1 measurement finds little concentration, **the per-site playbook may have extracted most of the cheap wins**. Remaining levers become cross-cutting (R1 Full de Bruijn IR symbol-table compression; Whippet GC; cache size caps) rather than per-site fixes. This isn't defeatist; it's recognising the lever may be elsewhere than the original "per-site" framing assumed.

---

## 2. Where the memory currently lives

### 2.1 hello.drvPath (post-#751/#752, well-decomposed)

| Bucket | Bytes | Status |
|---|---|---|
| v3_arena | ~604 MB | Bindings dominant; then Thunks, Closures, ListVecs |
| Boehm heap | ~403 MB | **99.9 % FREE** per #702 — Boehm is holding pages it doesn't need |
| elsewhere | ~76 MB | CompilationUnit bytecode + vector slack + fiber stack + SQLite + Boehm metadata; **un-decomposed** (T1.2 not run) |
| TOTAL | 1083 MB | Down from 1934 MB pre-#751/#752 |

The Boehm "99.9 % free" observation is striking and **underexploited**. If Boehm is holding 400 MB of pages with ~0.4 MB live, tuning Boehm to release pages aggressively could yield substantial reduction at near-zero implementation cost.

### 2.2 haskell-nix-example (NO published decomposition)

| Bucket | Bytes | Status |
|---|---|---|
| Total | **3003 MB** | 5.3× TW (569 MB) |
| Per-bucket | **UNMEASURED** | NIX_VM_STATS not run on HNE in the visible history |

**This is the first gap.** Before any further memory work, run NIX_VM_STATS on HNE. The infrastructure already exists; cost is ≤ 1 hour. Without it, every memory hypothesis on HNE is unanchored.

### 2.3 cardano-node M5 (last measured 919 MB, stale)

Strategic workload; no re-measurement despite multiple defaults flipped. See `DIRECTION_NOTE_2026-05-26.md` §3.3.

---

## 3. What's been tried

### 3.1 Successful (the per-site playbook, 2026-05-21 era)

| Fix | Win | Pattern |
|---|---|---|
| #748 mergeBindings empty-operand short-circuit | −14.6 MB hello | Fast path when one operand empty |
| #750 primIntersectAttrs two-pass | −386 MB hello | Reclaim over-allocation slack |
| #752 attrPosTable inline into Entry's pad slot | −849 MB hello | Eliminate entire side table |
| **#751 + #752 combined** | **1934 → 1083 MB (−44 %)** | **Cumulative** |
| #746 attribution instrumentation | (measurement, not direct win) | Drove the above |

**Lesson:** per-site attacks worked because they found workload-specific slack patterns. Single-commit landings; immediate RSS delta.

### 3.2 Falsified (recent, 2026-05-26)

| Attempt | Reason |
|---|---|
| A1b pointer-keyed merge cache | 0.03 % hit rate on HNE; cache key didn't capture reuse pattern |
| A1a Phase C v1 (Chain conversion) | Implementation falsified (details in commit) |
| A1a Phase C v2 | Same |
| A1a Phase C v3 (`633c971ee`) | Same |
| B2 nursery default-on on hello/firefox (`f2c254fd4`) | Wall + memory falsifier on those workloads; may still apply to HNE/cardano (untested) |

**Lesson:** structural representation changes (Chain) and GC-mode flips (B2) didn't land. Either the savings model was wrong OR the implementation hasn't found the right shape.

### 3.3 Permanently negative findings (from `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` §"Negative findings")

| Item | Why rejected |
|---|---|
| Lazy frame-locals | GC cost > savings |
| Lazy OP_ATTRS_REC_INIT | 95 % consumption rate; deferral doesn't help |
| Path-prefix pool | Breaks Tag::Slot stability |
| Bindings HAMT (Stage 11 candidate) | Iteration not hot enough |
| ListVec / Bindings `_pad` "savings" | `sizeof` proves 0 B saved (compiler auto-pads) |

These are CLOSED. Not in this doc's avenues.

---

## 4. Remaining avenues by category

### Category 1: Per-site attack on HNE (HIGHEST near-term ROI)

The #748/#750/#752 playbook *worked* by finding workload-specific slack patterns. **It has NOT been re-applied to HNE post-A1 measurement.** A1 ran BINDINGS_ATTR; the analysis identified mergeBindings as the headline hot site, leading to Phase C work. But:

- **No HNE-specific NIX_VM_STATS bucket decomposition** has been published. We don't know the Boehm/v3_arena/elsewhere split on HNE.
- **No T1.3 per-alloc-site for non-Bindings types** exists. Thunks, Closures, ListVecs may have hot concentrations invisible today.
- **No common-value tracer (T3.2)** exists. String duplication on HNE could be 10-30 %; with intern, immediate win.
- **Post-V3_RELEASE re-baseline** hasn't been done. V3_RELEASE claims −25-40 MB; not measured on HNE.

| Move | Effort | Expected yield |
|---|---|---|
| **HNE NIX_VM_STATS bucket decomposition** | ≤ 1 hr | Tells us where the 3 GB lives; gates everything else |
| **HNE NIX_V3_BINDINGS_ATTR with top-10 sites** | ≤ 1 d | Confirms or refutes "mergeBindings dominant" pattern on HNE |
| **T1.3 per-alloc-site for Thunks/Closures/ListVecs/Strings** | 1-2 d | Surfaces non-Bindings hot sites; templated #746 pattern |
| **T1.2 elsewhere bucket decomposition** | 1 d | Decomposes the residual elsewhere bucket per AR23 |
| **T3.2 common-value tracer (top-N strings/ints/paths)** | 1 d | Finds duplication; if > 10 % is duplicate, string-intern is easy win |
| **HNE post-V3_RELEASE baseline** | ≤ 1 hr | Confirms V3_RELEASE −25-40 MB on HNE |
| **HNE force-deep audit** | ≤ 1 d | Identifies whether IFD probe or callPackages chains over-allocate |

**Total ~4-5 days.** None of these have been done in the current memory-work arc. They are the lever-finding work.

### Category 2: Structural representation changes (alternatives to Phase C)

Phase C v1/v2/v3 falsified, so the SPECIFIC ChainBindings approach isn't shipping. But there are *other* structural changes:

| Target | Approach | Estimated effort | Estimated yield |
|---|---|---|---|
| **Revert Phase A's +8 B Bindings header** | If Phase C is genuinely abandoned, the kind/parent fields are pure overhead. Revert. | ≤ 1 d | ~8 MB on HNE (~1 M Bindings × 8 B) |
| **Permanently strip Thunk::forces / Thunk::shapeCell** | V3_RELEASE strips these conditionally; make them permanent | ≤ 1 d | ~20 MB on HNE estimate |
| **LambdaCore/Meta split** | LambdaDescriptor has ~85 B cold fields; cold path → side table | 2-3 d | ~5-10 MB |
| **ListVec immutable tail sharing** | `[1, 2, 3 :: rest]` shares `rest` across thunks | unknown; complex | unknown |
| **String interning at allocation** | Many duplicates if T3.2 confirms | 1-2 d | Depends on T3.2 finding |
| **cu/capturedWiths side table** | Per AR23 + DATA_STRUCTURE_AUDIT B1/B2: null fractions unverified; if > 90 % null, sparse representation wins | 2-3 d | ~20-40 MB if claim holds |

**Total range: 6-12 days for ALL Category 2 items.** Worth selecting based on which Week 1 measurements support.

### Category 3: GC-level changes

| Change | Effort | Notes |
|---|---|---|
| **Boehm tuning (`GC_set_free_space_divisor`, `GC_set_max_heap_size`)** | 1 d measurement spike + 1 d tune | Boehm 99.9 % free on hello.drvPath suggests huge headroom |
| Phase E v0.3 on HNE | 1 d | Per `project_stage4_v3_2026-05-21`: addresses force-walk tenured; B2 falsified on hello/firefox but never tested on HNE |
| Selective nursery (only on alloc-heavy workloads) | 2-3 d | Architecture: B2 fails blanket default-on; selective might work |
| Whippet GC (Stage 16 R6) | Multi-month | Trigger-gated per BOEHM_DEPENDENCY |
| Precise GC | Multi-year structural | Stage 16+ candidate |

**Boehm tuning is the unexpected high-leverage candidate.** The 99.9 % free claim was made in #702 (2026-05-21) and hasn't been investigated since. If Boehm holds 400 MB of empty pages, returning even half of them to the OS is a 200 MB win at near-zero implementation cost. Worth a 1-day measurement spike first.

### Category 4: Lifetime / scope changes (underexplored)

| Change | Status | Why interesting |
|---|---|---|
| **Closure escape analysis** | Not investigated | Many Closures short-lived; could stack-allocate or pool |
| Stage 4 v4 / let-floating resume (R10) | DORMANT | Could reduce closure capture sizes; 0 elisions historically but Phase B+C consumer infrastructure may now enable it |
| Per-Thunk lifetime tracking (T4.2 from PROFILING_IMPROVEMENTS) | Deferred | 3-5 days; would surface RETENTION patterns vs ALLOCATION patterns |
| Force-deep early release | Not investigated | If forceDeep retains intermediate Bindings unnecessarily, releasing earlier helps |

**The lifetime story is genuinely underexplored.** Every memory measurement to date has been ALLOCATION-site attribution. RETENTION may matter more — a Thunk that retains a 100 MB Bindings for its entire `Suspended` state IS the memory cost even if the allocation site looks innocuous. Worth instrumenting.

### Category 5: Cache hygiene (operability)

| Change | Effort | Notes |
|---|---|---|
| **Phase 4b EvalResults size cap + LRU** | 1-2 d | Per AR11; cache grows monotonically; HNE cache size during eval = unknown |
| CU disk cache LRU + size cap | 1-2 d | Same |
| Process-RSS Phase 4b opt-out for memory-pressure workloads | trivial — `NIX_V3_NO_IFD_IMPORT_CACHE_DISK=1` exists | Workaround documented |

**Important note for HNE measurement:** if Phase 4b cache grows DURING the HNE eval, the 3003 MB measurement includes growing cache. Run HNE WITH and WITHOUT `NIX_V3_NO_IFD_IMPORT_CACHE_DISK=1` to isolate.

### Category 6: Configuration / build

| Change | Effort | Notes |
|---|---|---|
| **V3_RELEASE default-on** (after wall confidence) | trivial | meson flag; currently opt-in via `-Dv3_release=true` |
| Tune `NIX_V3_FIBER_STACK_SIZE` | trivial | Pre-allocated; default may be over-sized |
| Tune `NIX_V3_MAX_HEAP` per workload | trivial | Already exists; documentation update |
| Remove unused features at compile time | varies | Audit needed |

### Category 7: HNE-specific (workload-targeted)

This is the most uncharted territory. The team has been treating HNE as "a workload that's too memory-heavy" rather than asking **what specifically in HNE's code paths drives 3 GB**.

| Investigation | Why | Effort |
|---|---|---|
| **What in haskell.nix overlays drives allocation** | HNE has 0 derivation primops but uses 3 GB — must be Bindings/Thunks/Closures from module system | 1-2 d investigation |
| **Module-system fix-point intermediate retention** | If fix-point retains all intermediate states, that's a TW pattern that v3 might amplify | 1-2 d |
| **haskell.nix's `callCabalProjectToNix` materialization** | HNE-class workloads materialize hugely; v3 may not release | 1 d |
| **HNE evaluation depth audit** | Deep recursion = many fiber frames = large stacks; cardano had 4096+ overlay layers | ≤ 1 d |

Note these aren't optimization targets directly — they're INVESTIGATION targets. They tell you WHERE the memory is going, which then enables Category 1-6 work to target precisely.

### Category 8: Cross-cutting structural (long-term)

| Change | Trigger / status |
|---|---|
| R1 Full de Bruijn IR | Per CR1_CR2_AUDIT §4 — conditionally fired; ~1 wk effort; symbol-table compression yields ~10-30 % off cold load (indirect memory benefit) |
| Whippet GC | Stage 16 R6; multi-month; trigger-gated |
| Persistent attrsets (HAMT) | Stage 11 candidate; falsified at hello.drvPath scale; revival trigger needed |
| Precise GC | Multi-year |
| Parallel eval (Stage 13 R5) | 9-15 months; R1 prereq per AR10 |

These aren't this-week items but should be in view.

---

## 5. Recommended sequence

### Week 1 — measurement-first (4-5 days, parallelisable)

**Day 1:**
- HNE NIX_VM_STATS bucket decomposition (≤ 1 hr; cheapest data we don't have)
- HNE post-V3_RELEASE re-baseline (≤ 1 hr)
- Boehm "99.9 % free" investigation on hello.drvPath: can we tune `GC_set_free_space_divisor` to release pages? (≤ 1 d)

**Day 2-3:**
- HNE NIX_V3_BINDINGS_ATTR analysis — does the mergeBindings dominant pattern hold? (1 d)
- T1.3 per-alloc-site for Thunks/Closures/ListVecs (1-2 d) — find non-Bindings hot sites

**Day 4:**
- T3.2 common-value tracer (1 d) — find string duplication
- T1.2 elsewhere bucket decomposition (1 d, parallel)

**Day 5:**
- HNE force-deep / lifetime audit (≤ 1 d) — surface RETENTION patterns
- Consolidate findings into "HNE attribution v2" doc

### Week 2 — targeted attacks based on Week 1 data

Decision points based on Week 1 outcomes:

| Week 1 finding | Week 2 action |
|---|---|
| HNE bucket shows v3_arena dominant + Bindings hot | Per-site attacks (Category 1 + 2) |
| HNE bucket shows Boehm dominant | Boehm tuning (Category 3, top item) |
| T1.3 finds Thunk/Closure concentration | Per-site Thunk/Closure fixes |
| T3.2 finds string duplication > 10 % | String interning (Category 2) |
| Boehm tuning shows page-release headroom | Ship the tuning |
| HNE attribution shows different pattern than hello | Workload-specific fixes |
| Phase C genuinely abandoned | Revert Phase A's +8 B header (Category 2 first item, ~8 MB) |
| Lifetime audit surfaces retention pattern | Revive Stage 4 v4 (R10) OR build closure-escape analysis |

### Strategic alternative (if Week 1 finds little)

If Week 1 measurement finds no concentration, no Boehm tunable headroom, no string duplication, no Thunk/Closure hot sites — the genuine conclusion is:

**Memory on HNE is structurally hard at the current representation level.** The per-site playbook may have already extracted most of the cheap wins (on hello.drvPath, #751/#752 took it from 1934 → 1083 MB). HNE may simply allocate more *evenly* across many small objects with no concentration.

In that case:
- Memory wins become long-term cross-cutting work (R1 / Whippet / Stage 13)
- Strategic narrative adjusts to "ship memory-positive where cheap, prioritize wall otherwise"
- The 5.3× HNE RSS gap becomes a published characteristic, not a fix target
- Memory-first-class framing needs honest update: "memory matters; some workloads structurally need more memory than TW"

This isn't defeatist. It's recognizing the lever may be elsewhere. The team's discipline has been to FALSIFY rather than ship marginal wins; the same discipline says "if we can't find a per-site lever after measurement, don't ship marginal fixes."

---

## 6. What the recent falsifications collectively suggest

Three Phase C variants + A1b cache + B2 nursery all falsifying within 48 hours means **the structural changes the team has tried don't fit HNE's allocation patterns**. The team's mental model of HNE's memory profile may be incorrect.

The next move shouldn't be a fourth Phase C variant. It should be MORE MEASUREMENT to surface what HNE's patterns actually are.

This is the same lesson as the Phase 4b RCA pattern — but applied to memory rather than wall:
- Phase 4b: implementation falsified → RCA revealed cache scope bug → fix surfaced wall lever
- Memory work today: implementations falsified → ??? → needs RCA equivalent

**The "RCA equivalent" for memory is the measurement-first Week 1 above.** Until it's done, more Phase C variants are unlikely to land.

---

## 7. Updates to existing docs

### NEXT_STEPS_2026-05-25.md

**§3 Tier A** — add A6:
- **A6:** Memory measurement-first week (HNE NIX_VM_STATS bucket + BINDINGS_ATTR + T1.3 + T3.2 + Boehm investigation). ~4-5 days parallelisable. Gates further memory implementation.

**§8.5 AR list** — implied updates:
- AR3 (Phase 4b inflates A1 RSS) — partially addressed by §4 Category 5 measurement
- AR11 (disk cache size growth) — surfaced as memory-relevant, not just operability

**§12 Operating rules** — add:
- "Memory measurement before memory implementation. Three falsified Phase C variants validate this rule."

### MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md

This new doc is the post-falsification follow-on. Original opportunities doc identified:
- Tier A measurement spikes (subset done; subset remains in Week 1 above)
- Tier B cheap wins (~90-140 MB; partially superseded by A1a Phase A/B/C work)
- Tier C medium wins (100-300 MB; partially attempted via Phase C)

Recommend tagging the original §2.1 ("shapeCell #ifdef") as LANDED (V3_RELEASE strips), and re-evaluating §3 Tier A measurement spikes against Week 1 plan above.

### ARCHITECTURE_CRITIQUE_2026-05-26.md

§7 AR list — confirms relevance of:
- AR3 (Phase 4b cache inflation during A1 measurement)
- AR23 (LambdaDescriptor non-derivable fields)
- AR25 (REC_SET / REC_INIT count validation)
- These are also memory-relevant in light of Category 7 investigation

---

## 8. Honest limits

- **All effort estimates are author's judgment, not validated.** Week 1's ~4-5 days could be 7 in practice; measurement infrastructure exists for most items but interpretation requires care.
- **"99.9 % free Boehm" claim** is from #702 (2026-05-21) on hello.drvPath. Whether this holds on HNE or post-#751/#752 is unverified.
- **"Per-site playbook may have extracted most cheap wins"** is my assessment, not the team's stated position. The team may have a strong intuition about specific remaining sites I'm not aware of.
- **Phase C variants 1, 2, 3 may have already informed Week 1's measurement.** If so, some Week 1 items duplicate already-done analysis.
- **HNE NIX_V3_BINDINGS_ATTR may have been run** but not surfaced in commit history. Check before re-running.
- **The "lifetime not allocation" hypothesis (Category 4)** is novel here; not supported by direct evidence. May or may not be the lever.
- **The "structural memory hardness" alternative (§5 strategic alternative)** depends on Week 1 outcomes — it's a contingency, not a current claim.
- **String interning may already exist** in some form (Symbol table is interning); the question is whether VALUE-level strings (not symbol names) are duplicated. Worth verifying before T3.2.
- **Whippet GC, persistent attrsets, precise GC** are listed for completeness; none are this-week items.
- **The "post-V3_RELEASE re-baseline" task** assumes V3_RELEASE has actually been built and tested on HNE. If only synthetic alloc-heavy bench was measured (per A2 commit), HNE measurement remains unverified.

---

## 9. Cross-references

This doc operationalises:

- [`MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md`](MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md) — original audit; Tier A spikes still relevant
- [`HNE_MEMORY_ATTRIBUTION_2026-05-26.md`](HNE_MEMORY_ATTRIBUTION_2026-05-26.md) — A1 measurement source
- [`DIRECTION_NOTE_2026-05-26.md`](DIRECTION_NOTE_2026-05-26.md) — RSS-stagnation observation that triggered this doc
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) — Tier A / Tier B / AR list; updates per §7 above
- [`ARCHITECTURE_CRITIQUE_2026-05-26.md`](ARCHITECTURE_CRITIQUE_2026-05-26.md) — AR list extension; T1.2 / T1.3 / T3.2 references
- [`PROFILING_IMPROVEMENTS_2026-05-24.md`](PROFILING_IMPROVEMENTS_2026-05-24.md) — T1.2 / T1.3 / T3.2 originally specified there
- [`WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md`](WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md) §6.1 — V3_RELEASE source spec
- `[[memory-first-class]]` — the framing this doc cross-checks; if Week 1 finds little, framing needs honest update
- [`GC_VS_TW_ANALYSIS_2026-05-23.md`](GC_VS_TW_ANALYSIS_2026-05-23.md) — B2 falsifier interpretation
- [`BOEHM_DEPENDENCY_2026-05-21.md`](BOEHM_DEPENDENCY_2026-05-21.md) — Boehm tuning + Whippet escalation criteria

**Commits referenced:**
- #748 mergeBindings short-circuit (−14.6 MB)
- #750 primIntersectAttrs two-pass (−386 MB)
- #752 attrPosTable inline (−849 MB)
- #702 RSS bucket decomposition
- `66b1061cd` A1 measurement
- `920cda88c` A1b FALSIFIED
- `98ca953bb` A1a Phase A scaffold
- `6f8095cd5` + `2cf14fdce` A1a Phase B v1/v2
- `633c971ee` Phase C v3 FALSIFIED + B2 FALSIFIED
- `f2c254fd4` B2 nursery default-on FALSIFIED
- `46ce47c8a` A2 V3_RELEASE LANDED (−8.4 % wall; RSS deferred)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
