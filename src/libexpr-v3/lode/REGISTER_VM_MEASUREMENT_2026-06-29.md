# Register-VM ceiling — measured (not estimated). 2026-06-29

Built a measurement tool to settle "would a register VM collapse the get/set/push traffic,
and by how much for K registers?" — instead of my earlier ~10–15% guess. Tool: per-instruction
frame-occupancy histogram (= locals+temps = Lua-style register-window pressure) +
collapsible-data-move split, dynamically weighted. Gated NIX_VM_OPCOUNTS. Model: cap the
register window at K; a data-move op at occupancy d folds into an operand iff d < K (its slot
is a register), else stays as a spilled memory access — EXACT for cap-K-window+linear-spill.

## Op-collapse curve (firefox 36.86M ops / M5 235M ops)

```
                       firefox                          M5
  collapsible ceiling: 48.8% of all ops               45.5%
  K=3   regs:  2.6% collapse ( 5% of collapsible)      2.8% ( 6%)
  K=5   regs: 10.1% (21%)                              8.9% (20%)
  K=7   regs: 19.5% (40%)                             19.1% (42%)
  K=9   regs: 24.2% (50%)                             24.6% (54%)
  K=12  regs: 27.3% (56%)                             30.2% (66%)
  K=16  regs: 33.4% (69%)                             33.9% (75%)
  K=24  regs: 36.8% (76%)                             39.3% (86%)
  K=32  regs: 38.3% (79%)                             40.3% (89%)
```

Occupancy is NOT low-pressure: a long tail to d12–18 (big attrset/list construction puts many
values live at once). So K=7 captures only ~40% of the collapsible; you need **K≥16** for ~75%.

## So YES — a register VM collapses a LOT of ops (the earlier ~10–15% was wrong at the op level)

At K=16 it folds ~34% of ALL opcodes (~75% of the data-moves); at the ceiling ~46–49%. The
user's intuition was right: a register VM does collapse most of the get/set/push.

## BUT the CPU win is modest — because the collapsed ops are the CHEAPEST

opcycles ranking by CPU time (firefox, top consumers): OP_CALL_PRIMOP, OP_ATTRS_SELECT_DYN,
OP_ATTRS_HAS, OP_TAIL_CALL_N, OP_STR_CONCAT, the call family. The collapsible moves
(GET/SET local/upval) are NOT in the top 12 by time. (opcycles absolute ns are clock-overhead
-inflated, but the ranking is robust and matches the reliable sample profile.)

Reliable CPU bound: the sample profile (project_profile_at_scale) measures DISPATCH at 7–23%
of CPU. The register VM removes (collapsed fraction) × dispatch + the value-stack memory
traffic of the folded moves. At K=16: ~34% of ops × ~15% dispatch ≈ ~5% + a few % stack-
traffic/I-cache ⇒ **~5–10% CPU win**. That narrows firefox 2.47× → ~2.25–2.35×. It does NOT
beat TW.

## Why — this is Reason B again

The CPU is dominated by the SEMANTIC ops a register VM doesn't touch: primop calls, attrset
select/has, function calls, string concat, and the alloc/GC-tax underneath them. Collapsing
the cheap data-moves removes op COUNT but little TIME. The register-VM question is real and
the collapse is large, but it lands on the cheap third of the work.

## Verdict

A register VM is a legitimate ~5–10% CPU narrowing (and needs K≥16 to get most of it), but it
does NOT close the gap to TW — the gap is the semantic/representation/GC-tax residual (Reason
B), not the stack-machine shuffle. If the shuffle is to be removed for real, it's as the
operand model of an OPTIMIZING JIT (register allocation in native code, fed by v3's IR), where
it composes with removing dispatch AND inlining — not as a standalone interpreter rewrite for
~5–10%.

Tool: allocStats.regPressureHist/regCollapsibleHist (alloc.hh) + dispatch-loop increment
(vm.cc) + projection dump (run.cc), gated NIX_VM_OPCOUNTS. Reusable for any workload.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
