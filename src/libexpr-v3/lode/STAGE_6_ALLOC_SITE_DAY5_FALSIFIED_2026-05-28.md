# Stage 6 Day 5 (continued) — alloc-site routing FALSIFIED

Per `STAGE_6_BLOCK_PROBE_DAY5_2026-05-28.md` recommended next step:
test whether allocation-time routing (alloc-site hint → young/old
region) could produce dead-block-friendly distributions naturally,
avoiding the need for runtime compaction.

## Probe added

`NIX_V3_BLOCK_PROBE=1` + `NIX_V3_BINDINGS_ATTR=1` + `NIX_V3_THUNKS_ATTR=1`
in combination dumps `reportAllocSites()`. Joins existing
`bindingsOriginTable` + `thunkOriginTable` per-site attribution against
the post-mark live sets, computes per-site allocated / live / live%,
sorts by allocated bytes.

## Pre-committed SHIP threshold

≥50% of allocated bytes come from "ephemeral sites" (live% < 10%).
If pass, alloc-time routing can recover them cheaply (route ephemerals
to a dedicated young region; let them die together).

## Results

### hello.drvPath

| Class            | Sites | Allocated  |
|------------------|------:|-----------:|
| Ephemeral (<10%) | 1,364 |    79.5 MB |
| Mid (10-50%)     |     3 |   small    |
| Persistent (>=50%) | 5,270 |  big      |

**Routing-candidate ratio: 17.2% — FAIL.**

Top sites by allocated bytes:
| Site                        | alloc | live  | live% |
|-----------------------------|------:|------:|------:|
| vm.cc:1229 [Bindings]       | 382 MB | 295 MB | 77.2% |
| vm.cc:3807 [Thunk]          |  54 MB |   2 MB |  3.1% |
| vm.cc:7426 [Bindings]       |   8 MB |   0.4 MB | 4.3% |

vm.cc:1229 (the generic Bindings allocator entry) dominates 70% of
the allocation; its bindings are persistent. Routing it to a young
region would not free them.

### HNE (haskell-nix-example `.packages.x86_64-linux.hello.drvPath`)

| Class            | Sites | Allocated  |
|------------------|------:|-----------:|
| Ephemeral (<10%) | 1,833 |    31.0 MB |
| Mid (10-50%)     |    39 |   small    |
| Persistent (>=50%) | 6,253 |  big      |

**Routing-candidate ratio: 3.0% — FAIL.**

Top sites:
| Site                        | alloc | live  | live% |
|-----------------------------|------:|------:|------:|
| vm.cc:1229 [Bindings]       | 585 MB | 376 MB | 64.3% |
| vm.cc:3807 [Thunk]          | 320 MB | 101 MB | 31.4% |
| vm.cc:7426 [Bindings]       |  45 MB |  16 MB | 36.3% |

The two biggest allocators (vm.cc:1229 + vm.cc:3807) account for
905 MB of HNE's 1577 MB arena. Both are mid-to-persistent. Routing
them to a young region would not free their bytes.

## Why this falsification matters

Both measurements unambiguously fail the >=50% threshold. The reason
is structural: the alloc sites at vm.cc:1229 + vm.cc:3807 are GENERIC
allocator entry points (`Alloc::allocBindings`, `Alloc::allocThunkSuspended`)
called from many contexts. The same site allocates BOTH long-lived
AND short-lived cells depending on caller-context.

`__builtin_FILE` / `__builtin_LINE` capture the allocator's own
source location, not the caller's. To distinguish lifetimes by
caller, we'd need callsite-threaded attribution (manual annotation
per call) or stack-inspection. Neither is cheap.

**Lifetime CANNOT be predicted at allocation time** in v3's current
architecture. To recover the dead bytes in HNE's mostly-half-full
blocks, runtime liveness tracking + relocation is required.

## What this leaves on the table

Day-5 has falsified two of the three Cheney alternatives from
`STAGE_6_CHENEY_FALSIFIED_2026-05-27.md`:

* (alt #1) Mark-sweep with fully-dead-block freeing — FALSIFIED
  (hello 2.9%, HNE 0%)
* (alt #2) Allocation-time generational routing — FALSIFIED
  (hello 17.2%, HNE 3.0%)

Remaining candidates per the falsification doc:
* (alt #3) **Mark + compact in place (sliding compactor)** — UNMEASURED
* (alt #4) Revert + pursue ImportCache LRU / other RSS strategies

**One new candidate emerged from Day-5 Block-Probe data:**
* (alt #5) **Selective per-block compaction** — UNMEASURED

Selective per-block compaction:
* Mark all reachable cells (like Cheney's mark phase)
* For each arena block, decide: skip (dense — >=50% fill) or
  evacuate (sparse — <50% fill)
* Live cells from sparse blocks copied to a compaction target pool
* Sparse source blocks freed; dense source blocks kept in place
* Forwarding map records sparse→target rewrites; dense cells keep
  their addresses

Estimated reclaim (from block-probe data, assuming <50% fill threshold):
* hello: 16 blocks under 50% → ~256 MB scanned → ~240 MB recoverable
* HNE: 55 blocks under 50% → ~880 MB scanned → ~610 MB recoverable

Peak during compaction (worst case):
* hello: 256 + 16 (target) = ~272 MB
* HNE: 880 + 270 = ~1150 MB

vs gate-OFF baseline:
* hello: 753 MB peak → 272 MB peak = -481 MB reduction
* HNE: 2279 MB peak → 1150 MB peak = -1129 MB reduction

Both would CLEAR the original SHIP gates from
STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md (≥200 MB hello / ≥600 MB HNE).

But this prediction relies on:
1. Mark phase correctly identifying live cells (we have this — same
   as Cheney's mark)
2. Block-fill measurement accurate (verified by Day-5 probe)
3. Pointer-rewriting correctness across sparse→target moves
4. String/path forwarding (we have this from Day-4)
5. Per-block decision threshold (<50% works on this data; future
   workloads may differ)

## Decision

Per Rule 0: alloc-site routing FALSIFIED.

Per measure-twice-cut-once §3.8: this is the second pivot on the
core Stage-6 premise ("generational like GHC RTS"). One more pivot
without measurement-passing data → falsification of the premise
entirely.

Per measure-twice-cut-once §3.7: the next direction (selective
per-block compaction) needs its own cheap measurement before
implementation, NOT direct coding.

**Recommended next step**: build a third probe — `NIX_V3_COMPACT_SIM=1`
that simulates the selective compaction algorithm on the current
mark + block-fill data and reports the actual peak during the
simulated copy (including the strings/paths forwarding load that
Day-4 measured at 956K bytes for HNE).

If simulated peak reduction clears the SHIP gates → implement.
If not → alt #4 (revert + pursue non-GC strategies).

This is the third measurement in the Stage-6 falsification chain.
Per measure-twice §3.8 "three failed pivots on same premise = full
falsification", a third FAIL would mean: ditch the generational/
compaction approach for v3's arena entirely, and pursue ImportCache
LRU / structural Bindings sharing / string interning as the RSS
levers instead.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
