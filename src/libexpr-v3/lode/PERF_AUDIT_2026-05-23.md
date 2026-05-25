# V3 Perf Audit — 2026-05-23

Five parallel agents reviewed the v3 VM for performance improvements:
hot-path overhead (debug prints / counter gates / getenv caches),
allocation hotspots per hot opcode, dead-code post-recent-retirements,
the 48.92 % local-stack-motion dispatch bottleneck identified by #778,
and GC / nursery / write-barrier overhead.

Synthesis below classifies findings into four tiers by risk × leverage.
Every finding cites file:line. Every recommendation carries a
falsifier per the measure-twice-cut-once rule. Three agent claims
were verified before inclusion; one was found inconsistent and
reframed.

## Headline summary

| Tier | Items | Combined estimated impact |
|---|---|---|
| **Tier 1 — Land today** | 2 items, ~111 LoC dead code | Zero risk, code-hygiene win |
| **Tier 2 — Cheap wins (1-3 days each)** | 4 items (3 bigram fusions + 2 alloc reductions) | ~32-50 ms wall / 4-8 % on hello.drvPath; ~950 MB allocation traffic reduced |
| **Tier 3 — Measurement spikes** | 3 items (slot-chain histogram, scavenge frequency, Phase E mortality) | Data-driven; gates Tier-2-style follow-ons |
| **Tier 4 — Confirmed no-action** | 4 items | Validates that recent hygiene work (#733, #767a, #720) was complete |

Total potential wall-time win Tier 1+2: **~32-50 ms (4-8 %) on
hello.drvPath**, plus the ~950 MB allocation-traffic reduction (CPU
cost on allocation paths, not residual RSS — nursery reclaims).

No bugs of the #733 class remain. The Phase D barrier work
(landed yesterday) is clean. The fork's hygiene infrastructure is
working.

---

## Tier 1 — Land immediately (clean dead code)

### T1.1 — Delete closure-pool / fakeClo dead code in alloc.hh

**Source:** Dead-code agent. **Verified:** `grep` of `.cc` files
returns 0 callers (the one match in vm.cc:5221 is a comment).

**Lines to delete (`include/v3/alloc.hh`):**
- `kPoolMaxBuckets`, `kPoolPerBucket`, `kFakeCloMagic` constants (lines 740, 742, 751)
- `tryPopFakeClo()` declaration + inline impl (lines 757, 789-809)
- `allocFakeClo()` declaration + inline impl (lines 762, 811-830)
- `recycleFakeClo()` declaration + inline impl (lines 769, 832-889)
- `ClosurePool` struct definition (lines 774-781)
- `threadClosurePool()` inline (lines 783-787)

**Total: ~110 LoC.**

**Retirement context:** Commit `ddb52d3a7` (#720 Step 12, 2026-05-21
15:34) retired the pool semantically when Phase D landed. The
inline implementations were left in the header — orphaned by the
retirement.

**Risk:** Zero. 0 callers in `.cc`; only references in `.hh` are
internal to the dead subtree.

**Falsifier:** Compile succeeds with the dead functions removed.
143/143 lang tests pass.

### T1.2 — Strip `NIX_V3_SKIP_INSTALLABLE_PREEVAL=1` from test scripts

**Source:** Dead-code agent.

**File:line:** `test/property/property_tests.py:540` still sets
the env-var that #764 made a silent no-op. Per `3af813638` (#760)
the var is retained one release as a compat shim; tests using it
are misleading because they suggest the var is load-bearing.

**Risk:** Zero. The env-var setting is now a no-op; removing the
line changes no behavior.

**Falsifier:** Test passes both before and after the removal.

---

## Tier 2 — Cheap wins with verified falsifiers (1-3 days each)

### T2.1 — Bigram fusion: `OP_SET_LOCAL_KEEP`

**Source:** Local-stack-motion agent. **Verified:** bigram data
from #782 (commit `cbfeb53e2`) shows SET_LOCAL → GET_LOCAL same-slot
at 7.19 % bigram frequency (~1.08M instances on hello.drvPath).

**Mechanism:** New opcode that writes the top-of-stack into a
local slot WITHOUT popping it. Pattern fuses `SET_LOCAL N; GET_LOCAL N`
(stash + immediately re-read) into one dispatch + one stack
operation instead of two of each.

**Estimated wall saving:** ~10 ms on hello.drvPath (1.3 % of
~785 ms eval). Per-fusion saving is ~25 ns (one dispatch + one
operand decode + one push + one pop, all eliminated).

**Effort:** 1 day. New opcode in `bytecode.hh`; one case in
`vm.cc` dispatch loop; pattern-match in `emit.cc` post-lowering
peephole.

**Falsifier:** `NIX_VM_OPCOUNTS=1 NIX_VM_BIGRAMS=1` shows
bigram count drops proportionally; hyperfine wall delta on
hello.drvPath ≥ 8 ms.

### T2.2 — Bigram fusion: `OP_GET_LOCAL_2` + `OP_GET_UPVALUE_2`

**Source:** Local-stack-motion agent.

**Mechanism:** New opcodes that read TWO locals (or upvalues) in
one dispatch. Bigram frequencies: GET_LOCAL → GET_LOCAL = 5.49 %
(0.82M); GET_UPVALUE → GET_UPVALUE = 4.23 % (0.63M).

**Estimated wall saving:** ~12 ms combined (~1.5 %).

**Effort:** 2 days (shared pattern-match infrastructure).

**Falsifier:** Same as T2.1.

### T2.3 — Eliminate `snapshotCurrentWiths()` allocation per thunk/closure

**Source:** Allocation-hotspot agent. **Verified:**
`vm.cc:1788` defines `snapshotCurrentWiths`, called from
OP_MAKE_THUNK (line 3625) and OP_MAKE_CLOSURE (line 3221) when
the lowerer didn't emit explicit `nWiths > 0` targets.

**Mechanism:** Currently allocates a ListVec per thunk/closure
made under a non-empty with-stack but without explicit with-targets.
On hello.drvPath: ~500K-1M ListVec allocations of 48-64 bytes
each = ~30-50 MB allocation traffic.

**Fix shape:** Emit with-targets from lowerer at all sites where
with-stack is active. Lowerer tracks `nWiths` per scope; pre-populate
captured-withs at compile time. The runtime snapshot becomes a
fallback only for synthetic closures (e.g., primop bridges).

**Estimated wall saving:** Difficult to quantify cleanly — allocations
are reclaimed by the nursery, so the saving is *allocation CPU
cost* not residual RSS. Likely 5-10 ms wall on hello.drvPath.

**Effort:** 2-3 days. Requires `lower.cc` instrumentation to
track active with-scopes at thunk/closure creation sites.

**Falsifier:** `V3_DBG_ALLOC_DUMP` `allocList` counter drops by
≥ 50 % on hello.drvPath. Wall delta ≥ 3 ms.

### T2.4 — Memoize fakeClo in Thunk to eliminate per-force allocClosure

**Source:** Allocation-hotspot agent. **Verified:**
`vm.cc:6409` synthesizes a fresh Closure per Suspended-thunk
force. With Phase D's nursery default-on, these are reclaimed
quickly, but the *allocation work itself* fires on 15M+ forces
on hello.drvPath. The closure-pool that previously amortized this
was retired in #720 Step 12 (yesterday).

**Mechanism:** Add `Closure * memoFakeClo;` field to `Thunk`.
First force synthesizes and stores; subsequent forces of the
same thunk reuse. Phase D barrier on the Thunk → Closure pointer
(infrastructure already exists).

**Estimated wall saving:** ~5-15 ms wall on hello.drvPath
(allocation overhead alone). Allocation traffic reduction:
~50 % of fakeClo allocs (since many thunks are force-once but
many also re-force).

**Effort:** 1-2 days. Closure pointer field; barrier on store;
read at OP_FORCE Suspended path.

**Falsifier:** `allocStats().closuresAllocated` drops noticeably
(target: ≥ 30 % reduction). Wall delta on hello.drvPath ≥ 3 ms.

**Caveat:** The closure-pool retirement narrative said
*"under Cheney nursery + Phase D, real generational reclamation
makes pooling redundant."* That's true for *memory*; per this
finding, it's not true for *allocation CPU*. The pool was doing
two jobs (memory + CPU); removing it solved memory but reintroduced
the CPU cost. The memoization fix is the cleaner alternative.

---

## Tier 3 — Measurement spikes (1-2 days each; gate further work)

### T3.1 — V3_DBG_CHASE histogram extension

**Source:** Local-stack-motion agent.

**Question:** Average slot-chain depth on hello.drvPath /
cardano-node M5. The #757 chase limit (kMaxIndirectionChase
raised 4096 → 100000) implies depths can be deep, but the
typical case isn't measured. If average ≥ 2, slot-chain
compression (write resolved WHNF back to visited slots) pays
~2-5 ms.

**Effort:** 1 day. Extend existing `V3_DBG_CHASE` (vm.cc:10785+)
to emit a depth histogram at process exit.

**Decision rule:**
- Average depth ≥ 3: implement slot-chain compression (~1 day)
- Average depth 1-2: skip; the chase already terminates fast
- Average depth ≥ 10 on cardano-node M5: investigate why; may
  be a separate bug class

### T3.2 — Per-category byte breakdown on hello.drvPath

**Source:** GC overhead agent.

**Question:** Which allocator dominates the 4 GB peak RSS on
hello.drvPath? `bytesBindings` / `bytesThunks` / `bytesClosures`
counters exist in alloc.hh:238-245 (per #702) but no published
breakdown for hello.drvPath specifically.

**Effort:** 0.5 day. Run `NIX_VM_STATS=1 v3-eval --file
hello.drvPath --strict` and capture the dump.

**Decision rule:** Whichever allocator dominates becomes the
next target for size optimisation. Likely candidates: Bindings
(per data-structure audit, average 1644 large attrsets at 365 KB
each = 600 MB of the 1.4 GB Bindings total).

### T3.3 — Phase E mortality measurement vs Phase D single-region

**Source:** GC overhead agent. **Verified:** Phase E v0.2
(commit `c4be4cfbc` #738) introduced two-region nursery with
age-based promotion. The v0.1 measurement spike documented
baseline mortality; the v0.2 post-landing benefit hasn't been
measured.

**Effort:** 1 day. Run hello.drvPath and cardano-node M5 with
`NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 NIX_V3_PHASE_E=0`
(Phase D single-region) and again with `NIX_V3_PHASE_E=1`.
Compare scavenge count, mortality rate, total RSS.

**Decision rule:** If Phase E shows ≥ 30 % mortality improvement,
keep default-on. If < 10 %, the two-region complexity isn't
justified; revert to Phase D single-region.

---

## Tier 4 — Confirmed no-action needed

The hot-path overhead audit, the largest by surface area, found
**no bugs of the #733 class remaining**. This validates the team's
recent hygiene work.

| Item | Why it's clean |
|---|---|
| Instrumentation counters (allocCount, forceCount, thunksForced) | Gated post-#733 (commit f36c9e29c) behind `dbgForceStatsActive()` |
| `++allocStats().bytes*` counters | Intentional permanent instrumentation per #702; cost is 1-2 ns per alloc; required for RSS decomposition |
| `++attrsetSizeBuckets[]` histogram | Intentional structural measurement for VM-2 sizing; runs once per attrset, not per-element |
| `phaseDActive()` checks at barrier sites | Promoted to `namespace inline const bool` per #767a; compiler hoists across consecutive barriers |
| `static const bool s_*` env-var caches | All getenv() calls in dispatch loop are static-initialised once at function entry; per-iteration cost is one cached-load |
| `fprintf` calls in primops.cc / vm.cc | All gated except `builtins.trace` (which is INTENDED to write to stderr) and `dbgLogForceSite` (which is gated at its callers) |

The cross-cutting `pattern is canonical` finding from Agent 1
captures it: the #733 fix established the right idiom, and every
subsequent diagnostic gate follows it. No regression.

The Phase D barrier work is also clean per Agent 5: 5-8 cycles
fast path per barrier site; `phaseDActive()` predicted-not-taken;
intra-tenured writes pay the inter-gen check but the cost is far
below the Boehm conservative-scan alternative.

---

## Discarded agent claims (verified inconsistent)

The GC overhead agent claimed Boehm scans v3 at 38 MB/s and
concluded "4 GB arena would take ~105 seconds to scan." Verified
against `project_702_hello_drvpath_rule0`: the 38 MB/s number was
*hypothetical scan bandwidth if Boehm were to trigger*, but
**Boehm rarely triggers** under v3-direct because the nursery
absorbs most allocation and Boehm's heap stays < 500 MB.

The 38 MB/s figure remains relevant for *future planning* (if v3
ever pushes Boehm's heap up significantly), but is not a current
bottleneck. Agent's "~105 seconds scan time" estimate was a
worst-case projection, not an observed cost. **Discarded.**

---

## Recommended sequence

Each step has a falsifier; the measure-twice-cut-once rule applies.

| # | Step | Effort | Risk | Falsifier |
|---|---|---|---|---|
| 1 | T1.1 — Delete closure-pool dead code | 0.5 day | None | `make` + 143/143 lang tests pass |
| 2 | T1.2 — Strip env-var from test scripts | 5 min | None | tests pass; grep returns 0 active readers |
| 3 | T3.2 — Per-category byte breakdown (informs T2 priorities) | 0.5 day | None | data table from `NIX_VM_STATS=1` run |
| 4 | T2.1 — OP_SET_LOCAL_KEEP | 1 day | Low | bigram count drops ~7 %; wall delta ≥ 8 ms |
| 5 | T2.2 — OP_GET_LOCAL_2 + OP_GET_UPVALUE_2 | 2 days | Low | bigram count drops ~9 %; wall delta ≥ 8 ms |
| 6 | T2.4 — fakeClo memoization | 1-2 days | Medium | `closuresAllocated` drops ≥ 30 %; wall delta ≥ 3 ms |
| 7 | T2.3 — snapshotCurrentWiths elision (lowerer change) | 2-3 days | Medium | `allocList` drops ≥ 50 %; wall delta ≥ 3 ms |
| 8 | T3.1 — V3_DBG_CHASE histogram | 1 day | None | data table |
| 9 | Conditional on T3.1 — slot-chain compression | 1 day | Low | wall delta ≥ 2 ms if depth was ≥ 3 |
| 10 | T3.3 — Phase E mortality measurement | 1 day | None | data table; informs Phase D vs Phase E default |

**Tier 1+2 total effort: ~7-9 days for the engineering work.**
**Expected combined wall saving: ~32-50 ms (4-8 % of hello.drvPath
~785 ms eval) plus the allocation-CPU reductions from T2.3 + T2.4.**

The team's recent velocity makes the math work: this is roughly
one week of focused work for ~5 % wall reduction, which is
material against the ~30× hello.drvPath gap.

## Honest read

This audit's main finding is what's NOT here:

- **No #733-class hygiene bugs** — the recent work has been
  thorough.
- **No Phase D barrier issues** — the fast path is the right
  cost.
- **No Boehm-as-bottleneck** (already falsified by #702).
- **No "deep architectural fix is the only path"** — Tier 1+2
  shows ~5 % wall reachable with engineering-shaped work.

The largest single lever (the 48.92 % local-stack-motion
bottleneck per #778) remains an architectural change (register VM
/ super-instructions). But the 7-9 days of cheap bigram fusions
plus the two allocation-reduction items shaves the dispatch
overhead by a measurable fraction without the architectural
investment. **That's good news**: it buys time for the bigger
architectural decision while the perf trajectory continues
improving.

Three measurement spikes (T3.1, T3.2, T3.3) gate further work
appropriately per the measure-twice rule. Each is ≤ 1 day. The
data they produce determines whether Tier-2-style follow-ons
are warranted.

---

## References

- Hot-path overhead pattern (#733): commit `f36c9e29c`, 2026-05-21
- Local-stack-motion bottleneck (#778): commit `fe7c17498`, 2026-05-23
- Bigram instrumentation (#782): commit `cbfeb53e2`, 2026-05-23
- Closure-pool retirement (#720 Step 12): commit `ddb52d3a7`, 2026-05-21
- Phase D default-on (#720 Step 11): commit `c0911aee6`, 2026-05-21
- Phase E v0.2 two-region (#738): commit `c4be4cfbc`, 2026-05-21
- Schema 9 sparse symbolTable (#781b): commit `23ec39c06`, 2026-05-23
- Schema 10 IC for OP_REC_BINDING_SLOT_REF (#779): commit `3a4b06ebc`, 2026-05-23
- Measure-twice rule: `feedback_measure_twice_cut_once.md`,
  `MEASURE_TWICE_CUT_ONCE_2026-05-23.md`
- Falsification rule: `feedback_falsification_rule.md`
- Per-category alloc bytes (#702): commit `1253bab2e`, 2026-05-20
- Data structure audit (related findings): `DATA_STRUCTURE_AUDIT_2026-05-21.md`

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
