# Ideal hand-rolled GC for v3 — design sketch + honest Whippet comparison

**Date:** 2026-05-26 (evening)
**Author:** session synthesis
**Status:** strategic — design exploration; no commitment, gives team a target if Whippet evaluation becomes serious
**Triggering question:** "What would the ideal hand-rolled GC look like for our VM?"

Companion docs:
- [`BOEHM_DEPENDENCY_2026-05-21.md`](BOEHM_DEPENDENCY_2026-05-21.md) — Whippet escalation criteria; R6 trigger
- [`GC_VS_TW_ANALYSIS_2026-05-23.md`](GC_VS_TW_ANALYSIS_2026-05-23.md) — current GC architecture (Phase A/C/D/E); decision rules
- [`NURSERY_PHASE_D_DESIGN_2026-05-18.md`](NURSERY_PHASE_D_DESIGN_2026-05-18.md) — write barriers
- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) — Category 3 GC-level changes
- [`EVAL_CACHE_ARCHITECTURE_2026-05-23.md`](EVAL_CACHE_ARCHITECTURE_2026-05-23.md) §4.3 — mmap'd L2 (composes with snapshot-region design here)

---

## 1. Position (TL;DR)

> **Framing rule (codified 2026-05-27 per `63c69536f`):** GC investments in v3 are **RSS-primary, not wall-primary**. Boehm consumes ~0 ms wall on real workloads (1 GC cycle / 0 ms total on hello.drvPath AND cardano-node M5). The wall case for replacing Boehm is dead. **The only load-bearing motivation for GC work is peak RSS reduction**, validated by direct measurement of the workload's freeable-memory ceiling.

**The ideal hand-rolled v3 GC would be a generational compacting precise-root GC with Nix-specific extensions** (Bindings interning, cross-process snapshot semantics, Thunk-aware collection). Total engineering: ~6 months. **Motivation: RSS reduction only — wall is not a target.**

**But Whippet exists and covers ~80 % of the design.** Whippet is a hand-rolled GC by Andy Wingo, designed for dynamic-language interpreters; its design space overlaps almost entirely with what v3 needs. Pragmatic answer: **Whippet + v3-specific extensions (~3-4 months total)** — gated on whether the RSS lever is large enough to justify the investment.

**Either path requires precise-root infrastructure as prerequisite.** That's 1-2 weeks of foundation work the team could start NOW without committing to custom-roll or Whippet. It's a no-regret investment.

**Concrete recommendation (updated 2026-05-27 post-spike):**
- ~~Boehm-tuning spike (1 day)~~ — **FALSIFIED on macOS aarch64** (`1285de2fe`); Boehm tuning didn't yield meaningful RSS on that platform
- Phase E v0.2 stress-mode missed-root fix (1-3 days) — still relevant
- Precise-root infrastructure foundation (1-2 weeks) — **LARGELY LANDED** (`6f854fa2c` + `02c95eba0` + `e7639f837`) in hours, not weeks
- **NEW: deregister arena blocks from Boehm (1-2 days)** — per `5865b807c`, this is the keystone enabling automatic Boehm GC and unlocking ~200+ MB peak RSS reduction
- Live-trace measurement landed (`f3491859f`): **239 MB freeable on hello.drvPath** measured directly — this is the validated lower-bound on RSS reduction

The new sequence is **measure → arena-deregister → automatic-collection → measure → commit-to-broader-GC-if-RSS-warrants**, NOT "ship Whippet for wall improvements that aren't there."

---

## 2. v3's workload-specific GC requirements

### 2.1 Measured allocation patterns

| Trait | Evidence | GC implication |
|---|---|---|
| **High allocation rate** | hello.drvPath: 6.5 M total; 25 K mergeBindings + 1.6 M Thunks + 350 K Closures | Allocation throughput is NOT the lever — v3 uses `threadArena()` (per-thread bump) and Boehm consumes 0 ms wall (`63c69536f`). **The high allocation rate matters for RSS (working set), not for wall.** |
| **High mortality** | Phase E v0.2: 42-57 % mortality on real workloads | Generational with young/old split is correct architecture |
| Mixed sizes | 16 B Values, 24 B Bindings::Entry, variable-FAM Bindings/Closures, multi-KB CU bytecode | Need both fixed-size + variable allocation paths |
| Deep object graphs | cardano-node 4096+ overlay layers; HNE deep callPackage; module-system fix-points | Mark phase must handle deep recursion without stack overflow |
| Long-lived data | CU disk cache, EvalResults, top-level flake outputs | Tenured generation must be efficient (compacting) |
| **Cycles common** | lib.fix, mutually recursive let, module fix-points | Tracing GC (mark-sweep / mark-compact) handles cycles natively; RC does not |
| **Cross-process determinism required** | Per #815 RCA | Disk cache loads must produce identical in-memory layouts |
| **Boehm 99.9 % free pages** | Per #702 (2026-05-21) | Conservative GC over-allocates; compacting + page release wins big |
| ~~**Boehm scan 38 MB/s**~~ FALSIFIED 2026-05-27 (`63c69536f`) | Boehm consumes 0 ms across full eval lifetime; scan rate is irrelevant when scan never runs | **Removed as a motivation.** Boehm-replacement-for-scan-rate is dead. |
| Single-threaded today | Stage 13 R5 considers parallel | Design for single-thread first; allow parallel as future extension |

### 2.2 Pain points in current stack

The existing GC stack is **Boehm conservative tenured + Cheney nursery opt-in (Phase A/C) + Phase D write barriers default-on + Phase E v0.2 two-region nursery opt-in**.

Observed pain:
- Boehm holds 99.9 % free on hello.drvPath (over-allocation; can't release)
- Conservative root scanning prevents object movement / pointer compression
- Phase E v0.2 known stress-mode missed-root (blocks default-on flip)
- B2 default-on flip falsified on hello/firefox (`f2c254fd4`)
- Cross-process determinism fragile (4 cache-coherence rules, Light variant brittleness)
- Phase D barriers convention-encoded, not statically enforced (per ARCHITECTURE_CRITIQUE §6 invariant table)

---

## 3. Ideal hand-rolled design

### 3.1 Core design principles

| Principle | Why |
|---|---|
| **Precise root scanning** | Unlocks compaction, pointer compression, cross-process snapshot |
| **Generational** | Matches observed 42-57 % mortality |
| **Compacting tenured** | Defrags; releases pages to OS (addresses 99.9 % free) |
| **Small per-object overhead** | 16 B Values are common; 1-byte header beats Boehm's ~16-byte |
| **Cycle-aware via tracing** | Nix has many cycles; RC + cycle-detection is more complex |
| **Bounded pause times** | Interactive `nix eval` UX needs <50 ms pauses |
| **Cross-process safe** | Snapshot regions for disk cache (composes with EVAL_CACHE mmap'd L2) |
| **Low write-barrier cost** | Phase D is on a hot path; cards beat per-pointer |
| **Eventual parallel safety** | Stage 13 future; design doesn't preclude per-mutator nurseries |
| **Inspectable** | Per-allocation-site attribution + mortality histograms + per-collection pause distribution |

### 3.2 Young generation (refinement of Phase A/C/D/E)

```
Layout:
  32 MiB nursery (16 MiB to-space, 16 MiB from-space, double-buffered)

Object header:
  1 byte total = [4-bit type tag] + [4-bit age bits]
  (vs Boehm's ~16-byte header)

Allocation:
  Bump-pointer: 3-5 ns / object
  Inline fast path: 5-10 cycles
  Slow path: scavenge nursery

Scavenge:
  Cheney algorithm (forward to to-space; update from-space pointers)
  Promote to tenured after N scavenges (Phase E v0.2 age threshold)

Write barriers:
  Card-table style: 4 KB cards × 1-bit dirty mark
  Per-mutator local cards; atomic flush to global remembered set
  Compatible with Phase D's current design
```

**Effort to refine current Phase A/C/D/E into this form: ~2-3 weeks** (mostly bug fixes + Phase E v0.2 stress-mode missed-root resolution + card-table flush optimization).

### 3.3 Tenured generation (custom mark-compact; REPLACES Boehm)

```
Layout:
  Page-based: 4 KB pages
  Large objects (> 1 KB): dedicated large-object space
  Free-lists per size class: 16 B / 24 B / 48 B / variable

Mark phase:
  Tri-color incremental (white/grey/black)
  Bit-pack mark bits at page header (one bit per word OR per object)
  Mark stack with overflow handling (work-stealing if Stage 13 active)

Sweep:
  Per-page free-list update
  Empty pages → release via madvise(MADV_FREE)
  (DIRECTLY addresses Boehm 99.9 % free)

Compact:
  Triggered when fragmentation > 30 % OR on RSS pressure signal
  In-place sliding within page
  Update remembered set + roots

Allocation:
  Free-list head pop for sized classes
  Page bump-pointer for variable allocations within page
  Large-object goes direct to dedicated space
```

**Effort to build from scratch: ~6-8 weeks** (significant; this is the hard part).

### 3.4 Cross-process disk cache integration

This is where v3 wins over generic designs:

```
Mmap'd disk cache region (per EVAL_CACHE_ARCHITECTURE §4.3):

  Treated as snapshot region — neither nursery nor tenured
  GC sees mmap'd region as immutable shared roots
  Objects in mmap region: refcount irrelevant (process holds reference via mmap)
  Cross-process safety: identical mmap → identical roots → identical layout

Copy-on-write:
  If consumer mutates an object loaded from mmap, COW to tenured
  Preserves snapshot region's read-only invariant
  Compatible with multiple-process simultaneous reads

Cache → tenured promotion:
  If a cache entry is touched repeatedly + mutated, promote to tenured
  Allows the cache region to release pages to other processes
```

**Effort: ~2-3 weeks** of integration work. **Composes naturally with mmap'd L2 cache design** (EVAL_CACHE §4.3); the GC and cache co-design here.

### 3.5 Precise root scanning (the load-bearing change)

This is what departs from Boehm conservative scanning. Requires:

```
1. Tag bits in Value distinguishing pointer / non-pointer
   - Already exists (16 B Value layout)

2. Stack maps for fiber frames
   - emit.cc generates per-opcode maps: { slot_idx → is_pointer }
   - Walk fiber stack at GC time using maps
   - Per-opcode map ~10-30 bytes; <1 % bytecode size overhead

3. Explicit root registration
   - GC_ROOT macro for C++ globals containing Value
   - Helper for STL containers holding Values (vector<Value>, etc.)
   - Audit existing GC_MALLOC sites in v3 code

4. VM dispatch state as precise root array
   - Already structured this way; just needs registration
```

**Effort: ~4-6 weeks** (touches many files; needs careful audit).

This unlocks:
- **Compaction** (movable objects)
- **Pointer compression** (could go to 32-bit pointers; halves Value size on 64-bit; potentially 30-50 % memory reduction if applied broadly)
- **Snapshot semantics for cache** (precise roots in mmap regions)
- **Eliminates Phase E v0.2 missed-root** structurally (precise scanning catches by construction)
- **Stage 13 prerequisite** satisfied (per AR10)

### 3.6 Nix-specific optimizations (what makes it CUSTOM)

These are what hand-rolling buys over Whippet:

#### 3.6.1 Bindings interning at GC level

Equivalent canonical-form Bindings deduplicate at allocation.

```
On Bindings allocation:
  Hash canonical form (sorted symbol-key signature)
  Lookup in intern table (weak references)
  If hit: return existing Bindings*; bump RC
  If miss: install new Bindings; weak-ref in intern table

GC handles intern table:
  Weak references; entries collected when last strong ref dies
  Compatible with mark phase (intern table = secondary roots)
```

**Estimated win on overlay-heavy workloads:** 10-30 % Bindings byte reduction on haskell.nix.

#### 3.6.2 Cross-process snapshot semantics

Per §3.4 above. Native to design.

#### 3.6.3 Thunk-state-aware collection

```
Thunks have a state machine: Suspended → Blackhole → Evaluated.
Suspended thunks that are PROVEN UNREACHABLE can be collected
without forcing them.

Today's GC treats all thunks uniformly (Suspended OR Evaluated).
An ideal v3 GC could:
  - Track Thunk state in object header
  - Suspended unreachable: collect normally
  - Evaluated unreachable: collect; release dependency chain
  - Blackhole reachable: defer (active eval)
```

This is the kind of integration only v3-team would build into a GC.

#### 3.6.4 Symbol-keyed object pool (cache locality)

```
Bindings entries grouped by symbol for cache locality.
Touching lib.foo causes 'lib' Bindings + 'foo' value to co-locate
on the same cache line during compaction.

Implementation: compaction phase uses symbol-table order to choose
relocation order. Identical lookup-pattern workloads get identical
cache behavior.
```

**Estimated win on lookup-heavy workloads** (lib.evalModules, callPackage chains): ~5-15 % wall via reduced cache misses.

#### 3.6.5 Per-allocation-site attribution NATIVE

Extension of #746 pattern; built into GC headers, not bolted on at instrumentation time.

```
Allocation header has 4-bit "site class" field (16 alloc classes).
Each site class maps to source line via static table.
GC can dump per-site bytes alive at any collection.
```

**Closes a class of measurement gaps** (T1.3 from PROFILING_IMPROVEMENTS becomes native).

#### 3.6.6 Per-collection lifetime tracking

```
At allocation: timestamp / scavenge-count tag
At collection: histogram of (age, type) pairs

Closes the "allocation measured, retention not" gap from
MEMORY_REDUCTION_AVENUES §Category 4.

Enables data-driven generational promotion thresholds:
  Phase E age threshold today is empirical
  An ideal GC tunes per-workload based on observed lifetime
```

### 3.7 Pause time / parallel safety

```
Pause time targets:
  Young gen scavenge: <5 ms typical
  Tenured incremental mark: <10 ms increment (bounded slices)
  Tenured compaction: <50 ms (acceptable for cache-miss-resolution)

Parallel safety:
  Per-mutator local nurseries (Stage 13 prereq)
  Write barriers per-mutator + lazy global flush
  Mark phase work-stealing (parallel marking)
```

---

## 4. Honest comparison to Whippet (Stage 16 R6)

Whippet is the existing escalation path per `BOEHM_DEPENDENCY_2026-05-21.md`. Real GC by Andy Wingo for dynamic-language interpreters. Already exists; community-maintained.

| Aspect | Hand-roll | Whippet |
|---|---|---|
| Young gen | Cheney bump-pointer (§3.2) | Same (mortality-tuned) |
| Tenured | Mark-compact (§3.3) | Immix (line-based; similar compacting) |
| Precise roots | Required (§3.5) | Required |
| Write barriers | Card table (§3.2) | Card table |
| Bindings interning (§3.6.1) | Native | Would require Whippet-extension hook |
| Cross-process snapshot (§3.6.2) | Native | Would require Whippet-extension hook |
| Thunk-aware collection (§3.6.3) | Native | Would require Whippet-extension hook |
| Symbol-keyed locality (§3.6.4) | Native | Custom integration |
| Per-site attribution (§3.6.5) | Native | Whippet has some; could extend |
| Lifetime tracking (§3.6.6) | Native | Partial in Whippet; extensible |
| Engineering effort | **~6 months** | **~3-4 months** (3 mo integration + 1-2 mo extensions) |
| Maintenance burden | v3-team forever | Andy Wingo + community |
| Documented design | Need to write | Existing |
| Battle-tested | No | Used in Guile / experimental ports |

### 4.1 Where hand-roll genuinely wins

The Nix-specific extensions in §3.6 (Bindings interning, cross-process snapshot, Thunk-state-aware, Symbol-keyed locality) are where custom shines. If these are CORE to the project's perf characteristics — not nice-to-have — then bolting them onto Whippet might be harder than building from scratch with them in mind.

**But this is speculative.** The team hasn't measured how much Bindings interning would actually save on haskell.nix. The Thunk-state-aware path hasn't been studied. Symbol-keyed locality is plausible but unquantified.

### 4.2 Where Whippet wins

- **5-7× less engineering effort.** Single biggest factor.
- **Maintenance externalized.** Andy Wingo + community handle the algorithm; v3 team focuses on extensions.
- **Battle-tested.** Used in Guile; design has been iterated.
- **Documented.** Clear design literature; team can read and learn from existing work.
- **Generic extensions.** Whippet's API allows hooks for custom collection policies.

### 4.3 The honest middle ground

**Whippet + v3-specific extensions is the pragmatic answer.** The 80 % shared design space means hand-rolling re-invents most of what Whippet provides. The 20 % v3-specific can be Whippet extensions where the API allows.

If the team needs deeper customization than Whippet's extension API allows, that's a future discovery — and switching from "Whippet+extensions" to "hand-roll" later is *much easier* than starting hand-roll now and migrating away.

**Whippet+extensions is the conservative-but-optimal path.** Custom hand-roll would only make sense if v3 has a property Whippet structurally can't accommodate — and the team hasn't established that.

---

## 5. What I'd reject

For completeness, design choices NOT in the ideal:

| Rejected | Why |
|---|---|
| **Reference counting (Bacon-Rajan)** | RC ops on every pointer write would crater allocation-heavy workloads. 1.6M Thunks/eval × 2-4 pointer writes each = massive overhead. |
| **Region-based + linear types (Cyclone/Rust)** | Requires static analysis. Nix is dynamically typed — no analysis can prove region disjointness reliably. |
| **Concurrent collection (Pauseless/C4)** | Overkill for current single-thread workload. Complexity not justified until Stage 13 R5 commits. |
| **Pure conservative (Boehm-like)** | Already there. Limitations observed (99.9% free, scan rate, missed-root risk) are inherent to conservative scanning. |
| **Pure stop-the-world mark-sweep without compaction** | Fragmentation accumulates; can't release pages. The 99.9% free problem persists. |
| **Generational with > 2 generations** | Phase E v0.2 mortality 42-57% suggests 2 generations (young + old) capture most benefit. 3+ generations adds complexity without proportional payoff. |
| **Per-thread heap (à la Rust thread-locals)** | Doesn't match Nix's single-thread eval pattern. Would force unnecessary copying. |

---

## 6. Foundations that should start NOW (no-regret)

Regardless of custom-vs-Whippet decision, these foundation items are required for either:

### 6.1 Precise root infrastructure (~1-2 weeks)

Start the work; bounded scope; no architectural commitment:

1. **Tag bits audit on Value layout.** Verify all 16B Values correctly distinguish pointer/non-pointer in their header.
2. **emit.cc stack map generation.** For each opcode, emit `{ slot_idx → is_pointer }` map. Adds <1% to bytecode size.
3. **GC_ROOT macro infrastructure.** Replace ad-hoc root tracking with explicit registration.
4. **STL container helpers.** `gc_vector<Value>`, etc. for containers holding Values.

### 6.2 Boehm tuning spike (~1 day)

Per MEMORY_REDUCTION_AVENUES Category 3: the 99.9% free observation has been visible since #702 but uninvestigated. Try `GC_set_free_space_divisor` + `GC_set_max_heap_size`. Potentially 100-300 MB recovery on hello.drvPath at near-zero cost.

### 6.3 Phase E v0.2 stress-mode missed-root resolution (~1-3 days)

Per `GC_VS_TW_ANALYSIS_2026-05-23.md` §4.2 — known issue; blocks any nursery default-on flip; would be inherited by any future GC if not fixed.

### 6.4 Selective nursery (~2-3 days)

B2 falsified blanket default-on; selective application (only on detected alloc-heavy workloads) might be wall-positive. Lower-cost than custom GC; resolves blocking issue.

### 6.5 Allocation-site attribution extension (~1-2 days)

Already partial via #746 BINDINGS_ATTR; extending to other types (T1.3) is no-regret regardless of GC choice. Both Whippet and custom benefit from having this data.

**Total foundation effort: ~3 weeks for items 6.1-6.5.** All compose; none require committing to custom-roll or Whippet adoption.

---

## 7. Two-tier recommendation

### Tier 1 — This-month foundation (~3 weeks)

All items in §6. Delivers:
- Immediate memory wins (Boehm tuning)
- Phase E v0.2 unblock
- Precise root foundation (prerequisite for any future GC)
- Extended attribution infrastructure

Decision-neutral. Reversible at any point.

### Tier 2 — When R6 (Whippet) trigger fires

Per `BOEHM_DEPENDENCY_2026-05-21.md`: nursery default-on lands AND tenured Boehm scan time > 10% of eval wall.

When triggered (currently not active):

1. **Integrate Whippet as tenured replacement** (~3 months integration)
2. **Add Bindings interning extension** (~2-3 weeks)
3. **Add cross-process snapshot integration with mmap'd L2** (~2-3 weeks)
4. **Maintain Phase D barriers (compatible)** — should drop in
5. **Add Thunk-state-aware collection if mortality data supports it** (~2-3 weeks investigation + impl)

**Total: ~4 months** Whippet+extensions. ~33% cheaper than custom hand-roll.

If at this point v3 discovers a property Whippet structurally cannot accommodate — that's the trigger to consider custom. Not before.

---

## 8. Honest limits

- **The §3.6 Nix-specific extension yield estimates are unmeasured.** Bindings interning "10-30%" is plausible based on overlay-heavy workload patterns but not measured. Same for Thunk-state-aware and Symbol-keyed locality.
- **Custom GC effort "~6 months"** is an estimate. Could be 4-8 months depending on team familiarity with GC engineering. Whippet integration "~3 months" is similarly approximate.
- **The cross-process snapshot design** in §3.4 composes with EVAL_CACHE mmap'd L2 but mmap'd L2 itself is deferred (R7 trigger-gated). The composition is theoretical until mmap'd L2 lands.
- **Phase D barrier compatibility with Whippet** is assumed; not verified. Real integration may surface mismatches.
- **Whippet's API may have changed** since the strategic doc set was written. Worth re-checking Whippet's current state before commitment.
- **"R6 Whippet trigger conditions"** assume nursery default-on lands AND tenured Boehm scan time > 10% of eval wall. Both could be a long way off; this design discussion may be premature.
- **Hand-roll vs Whippet vs status-quo comparison** ignores ecosystem factors (community Boehm tooling, debugger integration, OS compatibility). Some of these favor Boehm; some favor Whippet; few favor custom.
- **The pointer-compression mention (§3.5)** is speculative. Halving Value size is plausible but would touch every line of v3 code referencing pointers. Effort cost not captured in §3.5's "4-6 weeks."
- **Foundation work (§6) ROI** depends on whether Tier 2 ever triggers. If R6 stays dormant, the precise-root work may not pay off until much later. Boehm tuning + Phase E fix are clearly worth it regardless.
- **The "Whippet+extensions" is conservative-but-optimal" position** assumes Whippet's extension API is rich enough. The team should evaluate Whippet's actual API surface before committing.

---

## 9. Cross-references

- [`BOEHM_DEPENDENCY_2026-05-21.md`](BOEHM_DEPENDENCY_2026-05-21.md) — R6 Whippet trigger criteria; tier 2 path
- [`GC_VS_TW_ANALYSIS_2026-05-23.md`](GC_VS_TW_ANALYSIS_2026-05-23.md) — current Phase A/C/D/E architecture; Phase E v0.2 details
- [`NURSERY_PHASE_D_DESIGN_2026-05-18.md`](NURSERY_PHASE_D_DESIGN_2026-05-18.md) — write barrier design; card-table compatible with §3.2
- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) §Category 3 — GC-level changes; Boehm tuning, Phase E, selective nursery
- [`EVAL_CACHE_ARCHITECTURE_2026-05-23.md`](EVAL_CACHE_ARCHITECTURE_2026-05-23.md) §4.3 — mmap'd L2; composes with §3.4 cross-process snapshot
- [`PARALLEL_EVAL_CAPABILITIES_2026-05-18.md`](PARALLEL_EVAL_CAPABILITIES_2026-05-18.md) — Stage 13 R5; parallel-safety design (§3.7)
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) §8.5 AR10 — process-local SymbolId + parallel; R1 prereq for Stage 13
- [`ARCHITECTURE_CRITIQUE_2026-05-26.md`](ARCHITECTURE_CRITIQUE_2026-05-26.md) §6 — convention-encoded invariants; precise GC + static enforcement
- [`MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md`](MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md) §2.1 — shapeCell #ifdef (composes with §3.6 Thunk-state)
- Andy Wingo's [Whippet documentation](https://wingolog.org/tags/whippet/) — external reference for Tier 2 commitment

**Commits referenced:**
- #702 RSS decomposition (99.9 % free Boehm finding)
- `#705` Phase A/C Cheney nursery scaffold
- `c0911aee6` Phase D default-on
- `c4be4cfbc` Phase E v0.2 (two-region nursery)
- `f2c254fd4` B2 nursery default-on falsified
- `633c971ee` Phase C v3 falsified (related; structural change context)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
