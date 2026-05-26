# Bridge telemetry measurement spike — 2026-05-26 (late evening)

**Date:** 2026-05-26 (late evening)
**Status:** Falsifies #661 (per-process treeWalkerToV3 seen cache) via data
**Triggering question:** would a process-wide treeWalkerToV3 seen cache deliver enough wall improvement to justify the GC-aware implementation cost?

## Headline

**NO.** TW→v3 bridge time on HNE is 0.675 ms out of 4797 ms total eval wall = **0.014% of wall**. A seen cache can at MOST eliminate this time. Below the measurable noise threshold. Cache would have negative net value (implementation + maintenance + GC-tracking cost vs. effectively zero wall benefit).

## Measurement

Hyperfine-style HNE eval with NIX_V3_BRIDGE_TIMING=1 + V3_TIMING=1:

```
v3-direct timing (ms): lower=0.005 optimise=0.009 compile=0.008
                       run=4797.084 vm=4796.212 bridge=0.872
v3 bridge telemetry (count + nsTotal):
  tw->v3 full   count=61   ns=675122   (0.675 ms total, 11068 ns avg)
  v3->tw        count=11   ns=187082   (0.187 ms total, 17007 ns avg)
  tw force      count=8    ns=9332     (0.009 ms total, 1166 ns avg)
  TOTAL         count=80   ns=871536   (0.872 ms total)
```

Per-call: 11 µs avg for tw→v3 full conversion. Even if the seen cache eliminated 100% of repeat conversions (assume 50% hit rate), savings ≤ 0.34 ms out of 4797 ms = 0.007% wall.

## hello.drvPath comparison

```
v3 bridge-primop calls (TW->v3): __v3_call_bridge_1=0 __v3_force_attr=0 __v3_force_list_elem=0
```

Zero bridges. Seen cache contributes zero on standard nixpkgs workloads.

## #661 verdict

Per [[measure-twice-cut-once]]: pre-committed threshold for the seen-cache implementation should have been ≥1% wall improvement (the noise floor of casual hyperfine measurement). Measured potential: **0.014%**. Two orders of magnitude below threshold.

**Per [[falsification-rule]]: #661 is FALSIFIED. Should be closed without implementation.**

## Why bridges are now so rare

The decline in bridge frequency tracks the v3-NATIVE arc:
- Pre-Phase-4b: ~5000+ bridge calls per HNE eval (flake-input + IFD via TW)
- Post-Phase 4b default-on (`d22e1bfd3`): IFD imports use direct cache
- Post-v3-native callFlake default-on (`511074ff6`): flake outputs constructed natively
- Post-Phase E1+E2 (#804+#805): eager-bridge for small lists/attrsets eliminated

The remaining 61 bridges are concentrated in flake.lock fetch logic (TW's fetchTree builtin pulling `type` from v3-evaluated flake-input attrsets — see [[head-5-counter-trap]]).

## What this means for Phase E3

The 61 tw→v3 calls (no seen cache) and 11 v3→tw calls map to the bridge-primop counters:
- `__v3_call_bridge_1=8` ↔ 8 of the v3→tw calls
- `__v3_force_attr=51` ↔ subset of tw→v3 work (each force_attr drives a small tw→v3 conversion)
- `__v3_force_list_elem=0` ↔ no list-element forces

The 51 force_attr × 11 µs avg ≈ 0.56 ms — most of the 0.675 ms tw→v3 total. So the bridge primops ARE the cost source for the remaining bridge time. Retiring them WITHOUT removing the construction path doesn't help (the cost is in the force, not construction).

## Pre-committed retirement criterion (carry-over)

For Phase E3 to deliver measurable wall benefit:
- Total tw→v3 + v3→tw time must be > 1% of run wall
- Current: 0.018% — well below
- **Phase E3 retirement is correctness-only, not wall-positive**

This makes #806 E3 even less urgent than today's analysis suggested. The retirement is structural cleanliness, not performance.

## Cross-references

- [[head-5-counter-trap]] — measurement methodology rule
- [[same-host-bisect]] — parent operating rule
- [[measure-twice-cut-once]] — falsification methodology
- lode/FFI_AUDIT_2026-05-24.md §Category D — Phase E3 status

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
