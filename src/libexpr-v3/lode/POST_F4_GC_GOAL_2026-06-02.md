# GOAL: re-measure the post-bridge memory profile, then implement the GC the DATA demands

**Date:** 2026-06-02
**Status:** GOAL DOCUMENT — fires once F4 (bridge-apparatus deletion) lands. The headline target is unchanged (M5 < 4 GB watchdog); the GC *family* is chosen by re-measured data, NOT inherited from the bridge-era conclusion.
**Author:** session synthesis
**Triggering question:** "After the bridge is deleted, restart generational (non-moving) GC, or look at optimizations? What goal should we set, and implement a GC that fits the measured profile?"

Companion docs:
- [`TW_VALUE_ERADICATION_GOAL_2026-06-02.md`](TW_VALUE_ERADICATION_GOAL_2026-06-02.md) — F0-F5 (done) + F4 deletion (the precondition for this goal)
- [`GC_STRATEGY_INTEGRATED_2026-05-30.md`](GC_STRATEGY_INTEGRATED_2026-05-30.md) — the bridge-era 6-layer strategy + RSS targets (its L=0.74 premise is now obsolete; see §2)
- [`BRIDGES_HOLD_RETENTION_2026-05-29.md`](BRIDGES_HOLD_RETENTION_2026-05-29.md) — the 99.8%/99.9% finding this goal renders moot
- [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) — the L→GC-family mapping (§4 thresholds reuse it)
- [`L_MEASUREMENT_GAP_2026-05-28.md`](L_MEASUREMENT_GAP_2026-05-28.md) — `NIX_V3_LIVE_TRACE_PERIODIC` for L(t)
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §3/§3.8 — pre-commit thresholds; recalibration-only-on-falsified-premise

---

## 0. The headline goal (unchanged) + the new constraint on HOW

**PRIMARY GOAL: cardano-node M5 peak RSS < 4096 MB (under the 4 GB watchdog), from 5760 MB.** Unchanged since the GC strategy. The GC is a *means*; the watchdog is the *end*.

**THE NEW RULE: the GC family is derived from re-measured post-F4 profile data, not inherited.** The bridge-era conclusion "non-moving is required" rested on L=0.74, which was inflated by bridge-held bytes (99.8% of HNE live arena). Bridge deletion removes that input. **Re-derive; do not assume non-moving, do not assume GC is even needed.**

This is the anti-spiral mechanism. The previous arc produced **11 GC falsifications** by picking a mechanism from conviction and testing it. This goal picks the mechanism from *data*, with pre-committed thresholds (§4) decided before the numbers are seen.

---

## 1. Why the bridge deletion obsoletes every prior memory measurement

The bridge held **99.8% of HNE live arena / 99.9% of M5** (`BRIDGES_HOLD_RETENTION`). It did this by RECONSTRUCTING TW value graphs (nixpkgs-shaped attrsets, source-filter closures) that the bridge tables then pinned. Post-F4:

- Those reconstructed graphs are **never allocated** — the marshalling that built them is gone (fetchers/path/derivation/import return plain data built v3-native).
- So **peak RSS drops** by the bridge-reconstruction volume (M5 carried ~731 MB bridge-held; HNE ~518 MB).
- And **L drops** — the high-L bytes were largely bridge-held. The "non-moving required (L=0.74 → Cheney 2× peak)" conclusion specifically depended on this. At a lower post-F4 L, *copying GC reopens.*

**Every number that drove the GC strategy is now stale.** That is why this goal starts with re-measurement, not implementation.

---

## 2. Phase R0 — RE-MEASURE (the gate; ~1 day; NO GC code before this)

On the **post-F4 binary**, re-run the exact measurements that drove the bridge-era strategy. Pre-committed: **no GC implementation begins until R0 completes.**

| Measurement | Tool | What it decides |
|---|---|---|
| **M5 + HNE + hello peak RSS** | `bench/m5-cron.sh --full` + `NIX_VM_STATS` peak_rss | Is GC even needed? (Gate A) |
| **Bucket split** (v3_arena / Boehm / elsewhere) | `NIX_VM_STATS` RSS decomposition | GC problem vs cache problem? (Gate B) |
| **L(t) distribution** (L_min, L_median, L_max) | `NIX_V3_LIVE_TRACE_PERIODIC=K` | Which GC family? (Gate C) |
| **Live-fraction freeable** | `NIX_V3_LIVE_TRACE` | Can GC close the gap at all? (Gate D) |

Per [[same-host-bisect]]: same host, ≥3 runs, σ documented (M5 arena exceeds RAM → peak σ is wide; report the pooled-σ envelope).

### R0.1 — THE METRIC: committed/resident, NOT virtual (added 2026-06-03)

**The memory number is COMMITTED/RESIDENT bytes — what the OS charges the process against an OOM/watchdog limit — never requested VIRTUAL address space (VSZ / mmap reservations).** A process that reserves 1 TB of address space (GHC-style `mmap(PROT_NONE)` / `MAP_NORESERVE`) but touches 2 GB *uses 2 GB*; counting the reservation as 1 TB is wrong.

Concretely:
- **The metric = resident/committed pages**: Linux **cgroup `memory.current`** (the build-farm watchdog's actual basis) or `VmRSS` (`/proc/self/statm` resident field); macOS `task_vm_info.phys_footprint` (jetsam/Activity-Monitor basis) or mach `resident_size` (uncompressed resident, what `limits.cc` uses).
- **NOT the metric = VSZ / `VmSize`** — it includes every mmap reservation and is meaningless for "how much memory are we using."
- **Also not the metric = `v3_arena cumul` bump bytes** — that counts *every byte ever bump-allocated*, including freed/cold/never-faulted pages. R0 already saw this: M5 `v3_arena cumul = 7197 MB` vs `resident peak = 4686 MB` — the **4686 is the real number; 7197 is a diagnostic, not a memory figure.** The watchdog is on 4686-class resident, not 7197-class cumulative.

**Design license this grants the Nofl arena:** the collector MAY reserve a large *contiguous virtual* address space up front (clean block/line addressing, simple per-block indexing — the GHC/MMTk pattern) and commit blocks on demand, *without that reservation counting against the goal* — provided it (a) commits only touched blocks and (b) returns RSS on free. Caveat from R1: on macOS that return MUST be `munmap` (decommit-via-`madvise` is a no-op there), so a big reservation must be released by `munmap`-ing sub-ranges, not by madvise. Measure committed (touched-and-not-munmap'd), not reserved.

**Deliverable:** `POST_F4_MEMORY_PROFILE_<date>.md` with the four numbers (resident/committed, per the metric above) + the Gate verdicts below.

---

## 3. The decision the data makes (Gates A-D)

Pre-committed BEFORE R0 runs. The profile picks the path; conviction does not.

### Gate A — is GC needed at all?
- **M5 peak < 4096 MB** → **GOAL MET by bridge deletion.** No GC. Declare M5 unblocked; pivot to wall/determinism optimizations. (Plausible: the bridge *was* the memory problem.)
- **M5 peak ≥ 4096 MB** → continue to Gate B.

### Gate B — GC problem or cache problem?
Compute the over-watchdog excess `E = M5_peak − 4096`. Decompose where `E` lives.
- **"elsewhere" bucket (ImportCache + SQLite, GC-untouchable) ≥ 50% of E** → it is a **CACHE problem**. Route to cache eviction (Phase 4b LRU), NOT GC. GC cannot touch caches.
- **v3_arena dominates E** → **GC problem.** Continue to Gate C.

### Gate C — which GC family? (derived from L, NOT inherited)
Read L(t) distribution. (Thresholds from `GC_DESIGN_POST_CHENEY` §3 — Cheney peak = N(1+L); copying competitive only at low L.)
- **L_median ≥ 0.5 AND L_min ≥ 0.3** → **NON-MOVING** (mark-sweep + free-list, or Immix). Copying pays the 2× peak everywhere.
- **L_min < 0.3** (genuine low-L windows exist) → **COPYING/GENERATIONAL viable AT the low-L trigger.** Choose generational copying with trigger-at-trough (the trigger-policy lever the bridge-era never explored). Cheney was falsified at the *wrong trigger*, not categorically.
- **Bimodal / unclear** → run the Immix line-occupancy spike (`GC_DESIGN_POST_CHENEY` §5.1, ≥20% lines dead → Immix) to disambiguate.

### Gate D — can GC close the gap? (the freeable check)
- **Live-fraction freeable on M5 ≥ E** → GC can close the watchdog gap. Proceed to implement the Gate-C family.
- **freeable < E** → GC alone is insufficient; the residual is genuinely-live working set. Combine with allocation-reduction (per-site / strictness) or accept a raised watchdog. Do NOT over-invest in GC that can't reach the target.

---

## 4. Phase R1 — the cheap falsifier BEFORE the multi-week build (~2-3 days)

Every one of the 11 prior falsifications hit the **arena-page-release pin** (`alloc.hh` has no `madvise`/`munmap` — freed bytes never return to the OS). **Whatever GC family Gate C selects, it MUST release pages, or it repeats the 11 failures.** So before building the full GC, run the pin spike:

- **madvise/munmap spike** on the post-F4 arena: free a known-dead region, `madvise(MADV_DONTNEED)` / `munmap`, measure RSS via `vmmap`/`smaps`.
- **Pre-committed:** post-release RSS drops by ≥80% of the freed bytes on macOS aarch64 AND linux x86_64. If <30% → page-release doesn't work on the platform; the whole memory approach needs rethink (this is the deepest possible blocker — surface it for ~3 days, not weeks).
- **Combine with Gate D's freeable:** confirm there is ≥E of *page-aligned* dead arena to release (dead bytes scattered sub-page don't release without compaction).

This spike is the difference between "the GC family is right" and "the GC family is right AND it can actually move RSS on this allocator." Both must hold.

---

## 5. Phase R2 — implement the data-fitted GC

Only after R0 (profile) + Gate C (family) + R1 (page-release works):

- **Reuse the precise-root substrate** — Stages 1 (`tagIsPointer`), 3 (`walkAllV3Roots`/`RootVisitor`), 5 (`GcRoot`). **These are now CLEANER post-F4**: the bridge was a major root-complexity source (`bridge_root_registry`, bridge-table enumeration, conservative scanning of bridged values). With the bridge gone, precise rooting is more tractable — a real tailwind for restarting GC.
- **Build the Gate-C family** (non-moving mark-sweep+free-list / Immix / generational-copying-with-trough-trigger).
- **Page-release is part of the GC, not a separate step** (R1 proved the mechanism; wire `madvise`/`munmap` into the sweep/collect). The 11 failures each tested reclamation *without* page-release; do not repeat that.
- **Default-OFF gate during bring-up** (`NIX_V3_MAJOR_GC=1` exists); flip to default only after the SHIP gate.

### Pre-committed SHIP gate (R2)
- **M5 peak RSS < 4096 MB** (THE goal — under watchdog), ≥3 runs, pooled-σ envelope.
- **HNE + hello peak RSS** below the post-F4 baseline by the Gate-D freeable (recalibrate the bridge-era ≤1800 / ≤400 targets against the *new* baseline once R0 lands).
- **Wall regression ≤ 10% (hello/HNE), ≤ 15% (M5)**, ≥10 hyperfine runs.
- **drvPath byte-identical** on hello + HNE + M5; `--quick` 6/6 + `--core` 15/15; brute audit under stress.

### Falsification (clean exit, not another spiral)
- If R1 page-release spike fails (<30%) → the allocator, not the GC family, is the blocker; pivot to allocator redesign or raised-watchdog. Write the Rule-0 doc.
- If R2 SHIP gate misses M5<4096 after the family ships → re-measure E and the freeable; the residual is either cache (→ Gate B route) or genuinely-live (→ allocation reduction). Do NOT try a *different GC family* on the same profile without a new measurement justifying it (that is the spiral).

---

## 6. The anti-spiral contract (why this is different from the 11 falsifications)

| Prior arc (11 falsifications) | This goal |
|---|---|
| Picked a GC mechanism from conviction, then tested | **Picks the family from re-measured data** (Gate C), thresholds pre-committed |
| Tested reclamation without solving page-release → all hit the pin | **R1 proves page-release FIRST**; it's part of the GC |
| Measured against a bridge-inflated L | **Re-measures L post-bridge** (R0); the old L=0.74 is void |
| "non-moving required" became dogma | **non-moving is a Gate-C OUTCOME, not an input** |
| GC vs cache conflated | **Gate B separates them**; cache residual routes away from GC |
| No "is GC even needed" check | **Gate A**: bridge deletion may have already won |

The single most important discipline: **R0 before any GC code. The profile is unmeasured; the bridge deletion was the biggest memory change in the entire arc; every prior number is stale.**

---

## 7. The "or optimizations?" half — they're a different axis, can run in parallel

If Gate A says M5 is under watchdog (or Gate D says GC can't reach it), the memory war is effectively settled by the bridge deletion, and the team pivots to optimizations that don't compete with memory work:
- **Determinism** — content-addressed bytecode cache + AOT (`POST_PURE_PIPELINE_OPTS` §1/§3). The standout post-pure-pipeline unlock.
- **Correctness** — the strictness-parity audit (the 15 code-review findings; should happen regardless).
- **Wall** — the register-VM / local-stack-motion lever (48.9% of dispatch).

These are orthogonal to the GC question and could proceed concurrently with R0 (R0 is ~1 day of measurement, not a blocking arc).

---

## 8. Honest limits

- **R0's M5 number is the whole game and it's unmeasured.** Everything in §3 branches on it. Don't pre-judge; the bridge may have won (Gate A) or barely dented it.
- **L could be anywhere post-bridge.** The "copying reopens at low L" branch (Gate C) is real but unconfirmed — re-measure, don't assume either direction.
- **The page-release pin (R1) is platform-sensitive and may be the true blocker** regardless of GC family — it failed to translate to RSS in the bridge era. R1 surfaces this in ~3 days before any multi-week build.
- **Gate D may say "GC can't reach the target"** — if the post-F4 residual is genuinely-live working set, no GC frees it; the lever becomes allocation reduction (strictness, per-site) or a raised watchdog. Accept that verdict rather than over-investing.
- **The bridge-era RSS targets (HNE ≤1800 / hello ≤400) must be recalibrated** against the post-F4 baseline (per [[threshold-recalibration-rule]] — the load-bearing premise, the baseline, changed). Don't hold the team to bridge-era numbers.
- **This goal does not pre-commit to writing a GC at all.** If Gates A/B/D say no, the honest outcome is "bridge deletion settled it" or "it's a cache problem" — and that's a *success*, not a failure to implement GC.

---

## 9. Cross-references

- [[tw-value-eradication-goal-2026-06-02]] — F4 deletion is this goal's precondition
- [[gc-strategy-integrated-2026-05-30]] — bridge-era strategy (L=0.74 premise now void; §7 targets recalibrate)
- [[bridges-hold-retention]] — the 99.8%/99.9% finding this renders moot
- [[gc-design-post-cheney-2026-05-28]] — §3 L→family mapping (Gate C), §5.1 Immix spike
- [[l-measurement-gap-2026-05-28]] — `NIX_V3_LIVE_TRACE_PERIODIC` (Gate C tool)
- [[post-pure-pipeline-opts-2026-06-02]] — §7's optimization axes
- [[measure-twice-cut-once]] §3/§3.8 — pre-commit thresholds + recalibration rule
- [[memory-first-class]] — RSS-primary framing
- [[falsification-rule]] — R1/R2 falsification exits
- [[same-host-bisect]] — R0 measurement discipline
- Code: `alloc.hh` (no madvise/munmap — the pin R1 tests); `precise_root.{cc,hh}` (substrate, cleaner post-F4); `mark_sweep.cc` (gated GC scaffolding); `NIX_V3_LIVE_TRACE_PERIODIC` (L(t))

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
