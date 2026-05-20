# Session plan — Bindings polymorphism + Stage 4 strictness + Stage 3 nursery

**Date**: 2026-05-20  
**Context**: User asked to "finish the rest of all the steps necessary to clear Bindings
polymorphism, Stage 4 strictness, and Stage 3 nursery."  Each is multi-week per
ROADMAP_TO_VISION_2026-05-15.md.  This document tracks status across this and future
sessions.

## What landed this session (mega-session, 2026-05-20)

### Track A — Bindings polymorphism

**Phase A landed** (commit `0ff0aa8b3`):
- `Alloc::allocBindings(0)` returns a static `sEmptyBindings` sentinel (skips
  97 458 empty-Bindings allocs on hello.drvPath; saves 0.78 MB).
- Bindings size histogram exposed via `NIX_VM_STATS=1`:

  | bucket | count     | est. bytes |
  |--------|-----------|------------|
  | 0      | 97 458    | 0.78 MB    |
  | 1      | 718 289   | 23.0 MB    |
  | 2      | 526 923   | 29.5 MB    |
  | 3-4    | 199 210   | 18.3 MB    |
  | 5-8    | 729 703   | 120 MB     |
  | 9-16   | 225 136   | 69 MB      |
  | 17-32  | 141 728   | 84 MB      |
  | 33-64  | 328 223   | 385 MB     |
  | 65-128 | 1 513     | 3.5 MB     |
  | 129+   | 1 644     | ~600 MB    |

**Phase B (Single/Small inline shapes)** — NOT LANDED.  Realistic effort: 3-5 days.
The Bindings type is touched in ~hundreds of call sites (vm.cc, primops.cc,
lower.cc, v3_call_flake.cc, ir.cc, treeWalkerToV3, …).  Adding a tagged
polymorphic shape requires updating every consumer that reads `b->size` /
`b->entries[i]`.  Lower-risk path: keep `Bindings` external interface stable,
add a private "smallStorage" union — but the alignment/aliasing rules
inside a FAM struct are subtle.  Deferred.

**Phase C (deduplication for 129+ giant Bindings)** — NOT LANDED.  These
1 644 allocs consume ~600 MB (~365 KB each, avg ~15 K entries).  Likely they
are repeated reconstructions of the same nixpkgs-top-level attrset via
makeScope/extends overlays.  Identifying duplicate-content Bindings and
sharing them is a major correctness audit (mutation, identity semantics).
Deferred.

### Track B — Stage 4 strictness analysis

**Phase A landed** (same commit `0ff0aa8b3`):
- `isTrivialForLazy` now accepts empty `ExprList` and empty `ExprAttrs`
  (non-rec, no inherits, no dynamic) — TW already does the equivalent in
  `ExprList::maybeThunk`.  Saves 121 688 thunks on hello.drvPath (-1.2%);
  thunks bytes 800.2 → 790.8 MB (-9.4 MB).
- Cumulative effect: 10.04 M thunks / 3.30 M forced (33.6 % forced ratio).
  Still 6.75 M unforced thunks = 526 MB wasted on speculative thunkification.

**Phase B (provably-strict-position elision)** — NOT LANDED.  Requires:
- IR-level dataflow to identify positions where a value is GUARANTEED to be
  forced (operand of `+`/`-`/`*`/`/`/`<`/`>`/`==`, scrutinee of `If`,
  formal-arg of a `forceValue`-calling primop, etc.).
- Lower.cc emits `Eval` instead of `Thunk` at strict positions.
- Audit each strict-position rewrite for cycles (a rec-attrset sibling
  cycle that TW navigates via thunk-pointer indirection breaks if we
  eager-eval).  See `__lessThan` saga (#694) for the precedent.

Estimated effort: 5-7 days.  The cycle audit is the bottleneck — every
rewrite candidate must be falsifier-tested against the nylon repro suite.

### Track C — Stage 3 nursery default-on

**Phase A landed long ago** (`NIX_V3_NURSERY=1` routing, no scavenge).
**Phase C (scavenge)** landed; gated behind `NIX_V3_NURSERY_SCAVENGE=1`.
**Phase D (write barriers)** NOT LANDED.

Status quantified this session:
- `NIX_V3_NURSERY=1` (routing only): 143/143 lang tests pass; no perf change
  on hello.drvPath (no reclamation).
- `NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1`: hello.drvPath SIGSEGV (exit
  139) within seconds.  Scavenge correctness is the blocker.

The SIGSEGV is the Phase D necessity — intergenerational pointers (a
tenured object holding a nursery pointer that gets copied on scavenge) need
a write barrier to update the holder.  Without that, the holder dereferences
a stale nursery address after scavenge moves the pointed-to object.

Phase D MVP design exists at `NURSERY_PHASE_D_DESIGN_2026-05-18.md`
(three viable shapes — card-table, remembered-set, dirty-flag).  Picking
one + auditing each cell-set / Bindings-mutation / vm.cc store-into-attrs
site for the corresponding barrier write is multi-day work.

Estimated effort to land default-on: 7-10 days.

## Combined session deliverables

- 1 perf-trace tool + heap_trace daemon (`68dc90fca`)
- 1 Rule 0 measurement falsifying GC-scan dominance (`1253bab2e`)
- 1 doc registry catch-up — ERROR_UX + FFI_AUDIT (`ce75a584b`)
- 1 allocator-bytes instrumentation (`2654eca6e`)
- 1 Phase A empty-literal + empty-Bindings (`0ff0aa8b3`)

Total: ~10 MB allocator pressure reduction; 121 K thunks elided;
full alloc-attribution telemetry now in place; nursery + scavenge state
quantified; SIGSEGV-under-scavenge documented as Phase D blocker.

## Honest scope statement

Despite single-session intent, the three tracks have non-trivial completion
requirements (cycle audits, IR dataflow, write-barrier correctness).  Phase
A landed for each; Phases B-C require focused multi-day sessions per track.

This document is the handoff for those sessions.  Pick a track, find its
Phase B section above, the work is laid out.

## Rule 0 anchors per remaining task

| Task | Hypothesis it would kill / confirm |
|------|--------------------|
| Bindings Single/Small | "Inline storage for size 1-8 saves X% of Bindings bytes" |
| Bindings 129+ dedup | "The 1644 giant Bindings are duplicate-content" |
| Strictness elision | "X% of unforced thunks are at provably-strict positions" |
| Nursery Phase D | "Intergenerational pointer rate is low enough that a card-table works" |

## Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
