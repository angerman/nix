# M5 per-opcode cycle profile — 2026-05-27

**Date:** 2026-05-27 (morning, after m5-cron landed)
**Status:** measurement-only — identifies the next-actionable optimization levers
**Companion docs:** [`M5_CRON_2026-05-27.md`](M5_CRON_2026-05-27.md) (drift detection), [`IFD_S4_FALSIFIED_2026-05-27.md`](IFD_S4_FALSIFIED_2026-05-27.md) (IFD perf path falsified), [`BRIDGE_TELEMETRY_2026-05-26.md`](BRIDGE_TELEMETRY_2026-05-26.md) (bridge wall 0.018%)

## Why this matters

After today's falsifications (IFD S4, seen cache, bridge primops downgrade), the remaining ~26.5 s of M5 warm wall is concentrated in pure VM dispatch + primop execution. Per `[[measure-twice-cut-once]]`, before any multi-week optimization work, surface WHERE the cycles actually go.

## Method

```
NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_NATIVE_CALL_FLAKE=1 \
NIX_VM_STATS=1 NIX_VM_OPCOUNTS=1 NIX_VM_OPCYCLES=1 \
  nix eval --impure --expr '<cardano-node M5 expr>'
```

Per-opcode cycle attribution is collected at dispatch boundaries (#786). Measurement overhead is ~10-20 ns per dispatch; relative comparisons across opcodes are reliable, absolute times are inflated.

## Headline cycle distribution (M5, 600 M dispatches, ~26 s wall)

| Opcode | Total cycles | % | Count | Avg ns/op |
|--------|-------------:|--:|------:|----------:|
| **OP_TAIL_CALL** | 244.3 G ns | **33.0%** | 16,359,763 | 14,935 |
| **OP_CALL_PRIMOP** | 146.7 G ns | **19.8%** | 2,401,763 | 61,097 |
| **OP_ATTRS_SELECT_DYN** | 107.5 G ns | **14.5%** | 377,173 | **284,883** |
| OP_ATTRS_HAS | 79.7 G ns | 10.8% | 4,355,344 | 18,309 |
| OP_CALL | 60.5 G ns | 8.2% | 25,641,349 | 2,360 |
| OP_RETURN | 38.3 G ns | 5.2% | 34,339,122 | 1,115 |
| OP_STR_CONCAT | 36.9 G ns | 5.0% | 2,558,688 | 14,440 |
| OP_ATTRS_SELECT | 9.4 G ns | 1.3% | 15,643,378 | 604 |
| OP_GET_LOCAL_FORCE | 2.4 G ns | 0.3% | 4,043,583 | 604 |
| OP_GET_LOCAL | 2.4 G ns | 0.3% | 116,249,320 | 21 |
| OP_GET_UPVALUE | 1.9 G ns | 0.3% | 91,835,787 | 21 |
| OP_LENGTH | 1.8 G ns | 0.2% | 3,611,608 | 487 |

**Top 4 opcodes = 78 % of measured wall.** The cycles are CONCENTRATED.

## Critical interpretive note

`OP_TAIL_CALL` and `OP_CALL_PRIMOP` are control-transfer instructions. Their per-op "cycle attribution" includes time spent inside the called body until the next instrumented dispatch. So:

- **OP_TAIL_CALL 33 %** ≈ "time spent inside tail-called Nix lambda bodies" — this is most of the actual evaluation work.
- **OP_CALL_PRIMOP 20 %** ≈ "time spent inside C++ primop implementations" (genList, match, length, etc.).
- **OP_ATTRS_SELECT_DYN 14.5 %** ≈ "time spent computing dynamic attr names + intern+lookup" — this is a pure dispatch-internal cost.

This means the breakdown maps to:
- ~33 % Nix function bodies (eval-side)
- ~20 % primop bodies (C++ side)  
- ~14.5 % dynamic attr-lookup dispatch
- ~10.8 % static attrs-has dispatch
- ~22 % everything else (call setup, return, etc.)

## Next-actionable levers (ranked by ROI)

### Lever 1 — OP_ATTRS_SELECT_DYN deep-dive (highest ROI)

**285 µs per call is anomalously high.** Static OP_ATTRS_SELECT is 604 ns/op — 470× faster per call. The DYN version's overhead suggests:
- Per-call symbol interning of the computed name string
- Cache misses on hot symbol tables
- Per-call vector resize / lookup over many candidate attrsets

Spike (2-3 days): instrument the OP_ATTRS_SELECT_DYN handler to bucket time into (string-build / intern / binary-search / not-found-path). The 285µs/op is begging for a 10×+ reduction; even 10× would save ~3 s wall on M5 = 11 % wall improvement.

**Pre-committed SHIP threshold** (per [[threshold-recalibration-rule]]): if instrumentation surfaces a single concentrated cost ≥50 % of the 285 µs (e.g., "intern cost is 150 µs/op") that has a known cheap fix (e.g., pre-interned names from the bytecode), ≥5 % wall improvement is achievable. SHIP if ≥3 %.

### Lever 2 — Hot primop bodies (genList, match, length, elemAt)

OP_CALL_PRIMOP 20 % wall = 5.2 s of M5 wall in C++ primop implementations. The top callers from the primop-count list:
- `genList` 620,556 calls (per-primop attribution from #788 would surface the wall share)
- `match` 463,451
- `length` 441,538
- `__elemAt` 312,138

Spike (1-2 days): re-run with `NIX_VM_PRIMOP_TIMING=1` (per #788) to get per-primop wall attribution. Target whichever primop's avg-ns × calls is highest.

Risks: primop implementations are battle-hardened C++; perf-critical changes have correctness blast radius. Bound any optimization to ≤200 LoC + before/after parity sweep + measurement gate.

### Lever 3 — OP_TAIL_CALL setup overhead

OP_TAIL_CALL avg 14,935 ns/op INCLUDES called-body time, so it's not directly optimizable. But the SETUP cost (frame management, upvalue capture, etc.) is a separable share. If setup is 1-2 µs of the 15 µs, optimizing setup yields 7-13 % of OP_TAIL_CALL wall = 2-4 % of total wall.

Spike (3-5 days, harder): instrument inside the OP_TAIL_CALL handler to separate "setup" from "body". Compare with OP_CALL (non-tail-call, 2360 ns/op). The DIFFERENCE between OP_CALL and OP_TAIL_CALL is the inlining/elision win that tail-call enables — if it's already minimal, OP_TAIL_CALL setup is the floor cost.

### Lever 4 — OP_ATTRS_HAS dispatch

10.8 % wall, 18 µs/op. The `hasAttr a x` operation is high-frequency in module-system code (cardano-node uses NixOS-class modules). If a fast-path can be added for "attribute exists in same Bindings" (no traversal needed), this could yield significant savings.

Spike (2-3 days): profile OP_ATTRS_HAS callers; categorize by attrset size + whether key is present. Add a constant-time fast path for the common case.

## Anti-pattern warnings (per [[falsification-rule]])

- Do NOT pursue OP_GET_LOCAL / OP_GET_UPVALUE optimization based on raw call counts (116M / 91M). Their per-op time is 20-21 ns — that's measurement-overhead-dominant. Optimizing the body would have NO measurable wall impact because the bottleneck is the dispatch infrastructure cost, not the body.

- Do NOT assume "more cycles in OP_X = OP_X needs optimization." For control-transfer opcodes (OP_TAIL_CALL, OP_CALL, OP_CALL_PRIMOP, OP_RETURN), the cycle attribution includes called-body time. The optimizable share is only the SETUP cost.

- Do NOT publish "M5 is X% in opcode Y" claims without explicit overhead-adjustment. The measurement infrastructure inflates absolute times by ~20× on hot paths.

## Pre-committed retirement criterion (each lever)

Per `[[threshold-recalibration-rule]]` and `[[measure-twice-cut-once]] §3.8`:

- **Lever 1 (OP_ATTRS_SELECT_DYN)**: SHIP if ≥3 % wall improvement on M5 quiescent host. REVERT WITH DATA if < 1 %. TUNE if 1-3 %.
- **Lever 2 (hot primop)**: SHIP if ≥2 % wall improvement on M5 OR ≥5 % on the targeted primop's calling workload. REVERT if < 1 %.
- **Lever 3 (OP_TAIL_CALL setup)**: SHIP if ≥2 %. REVERT if < 1 %. Highest risk-of-correctness, so threshold is paired with explicit parity sweep on lang + property + nixpkgs.
- **Lever 4 (OP_ATTRS_HAS fast path)**: SHIP if ≥2 %. REVERT if < 1 %.

## What this does NOT do

- No optimization implemented this session. Pure measurement deliverable.
- No commitment to any of the 4 levers. Each is a separate focused spike.
- No claim about which lever the team SHOULD pursue first; ROI is estimated, not measured.

## Cross-references

- [[bridge-telemetry-2026-05-26]] — falsified bridge surface as perf target
- [[ifd-s4-falsified-2026-05-27]] — falsified IFD caching as perf target
- [[threshold-recalibration-rule]] — derivation methodology applied here
- [[measure-twice-cut-once]] §3.8 — anti-pattern guardrails
- [[falsification-rule]] — Rule 0; each lever has its own retirement criterion
- lode/M5_CRON_2026-05-27.md — drift-detection infrastructure
- `#786` — original opcycle attribution work; this is a fresh M5-targeted run

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
