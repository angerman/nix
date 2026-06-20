# v3 VM — in-depth profiling at scale (2026-06-21)

What the v3 VM *actually does* during large-workload evaluation, and the
data-driven list of candidate next levers. All numbers measured on the quiet
host **darwin-4** (aarch64-darwin), v3-direct, cache-off
(`NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1`), at HEAD `81b58dc66`.

Method: macOS `sample` (1 ms statistical sampler) for CPU self-time attribution;
existing in-VM counters (`NIX_VM_OPCOUNTS`, `NIX_VM_PRIMOP_TIME`, `NIX_VM_STATS`,
`NIX_VM_CACHE_SITES`) for the deterministic dynamics. Sample counts exclude
idle/wait threads (`__psynch_cvwait` etc. — 55k–66k samples of pure idle were
removed; on-CPU totals: M5 4299, firefox 2492, HNE 4464).

## TL;DR — the prior narrative was wrong
The 2026-06-19 structural analysis said the gap "lives in forceValue +
callClosure + dispatchLoop." **At scale, `forceValue` is ~2 % and `callClosure`
is <1 % of on-CPU.** They are NOT the bottleneck. The real cost distribution:

| category | M5 | firefox | HNE | what it is |
|----------|----:|----:|----:|-----------|
| **ALLOC** | 20.8% | 19.4% | 24.3% | malloc/free/memmove/memset — churn + std:: containers + block alloc |
| **BINDINGS** | 2.9% | **18.6%** | **16.4%** | ~all `Bindings::countDistinct` (chain walk) |
| **PARSE** | 11.0% | 15.2% | 10.4% | yylex / Parser::parse / addAttrLeaf |
| **DISPATCH** | 22.9% | 15.5% | 7.1% | the bytecode loop (per-opcode work) |
| **LOWER+HASH** | 8.8% | 7.8% | 18.7% | IR build / optimise / symbol intern / emitBlock |
| **TLS** | 6.1% | 3.6% | 1.5% | `_tlv_get_addr` (macOS thread-local access) |
| **GC** | 6.7% | 0.6% | 4.3% | mark / scavenge / visit |
| **FORCE+CALL** | 2.6% | 2.4% | 0.6% | **not the bottleneck** |
| OTHER | 17.7% | 15.9% | 15.4% | strings, sha256 (drv hash), std::set, value-stack |

OTHER decomposes to: string ops (`strlen`, `printString`, `lookupStringContext`,
murmur/cityhash), more lowering (`emitBlock`, `lowerExpr`), `sha256_block_armv8`
(the real drv hash — shared with TW, unavoidable), std::set/tree balancing,
value-stack `vector::__append`.

## VM dynamics (deterministic)
**Opcode histogram** (consistent across all three; M5 = 225 M ops total):
`OP_GET_UPVALUE` 13–17 % ≫ `OP_SET_LOCAL` 10–12 % > `OP_GET_LOCAL` 9–10 % >
`OP_RETURN` 7–8 % ≈ `OP_MAKE_THUNK` 7–8 % > `OP_GET_LOCAL2` 6–7 % >
`OP_ATTRS_SELECT` 2.6 %(ff)/7.4 %(M5) > `OP_BRANCH_FALSE` 3–4 %. The VM is
dominated by **variable access (upvalue/local) + thunk creation** — the
irreducible shape of lazy-FP-with-closures.

**Thunk churn (the elephant):** allocated-but-never-forced thunks =
**firefox 61.5 %, M5 65.4 %, HNE 67.4 %**. v3 builds ~3× the thunks it forces.
This single fact drives BOTH the #1 CPU cost (ALLOC) AND the #1 memory cost
(thunks 637 MB on M5). It is the residual *after* the shipped strictness pass
(`opt_strict_call_unthunk.cc`, default-on).

**Nursery routing is starved:** hit-rate **firefox 16.5 %, M5 7.3 %**, only 2–3
scavenges per eval, young mortality 60–63 %. The scavenge fires only at the
outermost dispatch level (`exitDepth==0`); in deep evals it rarely fires, so the
32 MB nursery fills once and ~85–93 % of allocations bypass to the arena.

**Caches:** capWiths-intern 99.3 % (healthy); AttrSelect IC <10 % (the
already-killed PIC — do not revisit).

## Methodology caveat — cache-off overstates the gap
PARSE + LOWER + HASH ≈ **20–29 %** of cache-off on-CPU. This is the parse→IR→
optimise→bytecode pipeline that the *disk cache amortizes* in real repeated
evals (and that TW does not have in the same form). So the cache-off 3.18× (M5)
**overstates the steady-state eval-loop gap.** Recommend adding a cache-ON
measurement to isolate the pure eval-loop ratio (caveat: cache-on changes the
drvPath-hash work profile — measure both, report both).

## Ranked candidate next-levers

### L1 — eliminate the redundant `countDistinct` chain walk  ★ TOP PICK
- **Size:** ~17 % firefox / ~16 % HNE on-CPU (the single biggest *named* v3
  function; firefox 428 vs mergeBindings 36 samples). Small on M5 (~3 %).
- **Cause:** `countDistinct` is O(N·depth) on Chain-kind Bindings; `OP_UPDATE`'s
  `mergeByCursor` calls it twice per `//` to size the output, **then walks both
  inputs again** to merge — and shared base chains get re-counted by every
  consumer's `//`. nixpkgs/haskell.nix are saturated with `//`.
- **Lever:** (a) FUSE — size the merge by the sum-of-layer-sizes upper bound and
  record the actual count during the merge walk, eliminating the pre-count walk
  in `mergeByCursor` + the chained-RHS-rescue; (b) MEMOIZE the distinct-count on
  immutable Chain Bindings (helps valueEqual/print/serialize/primops + the
  repeat-merge pattern).
- **Risk:** LOW — pure count, byte-identical by construction, no laziness change.
- **Falsifier:** implement gated, grade firefox/HNE CPU on darwin-4 min-of-5;
  expect ~8–15 % CPU drop. If not, the walk wasn't the cost (count is cheap, the
  *merge* walk dominates) → fall back to memoize-only.

### L2 — enlarge the nursery (rare-collect regime)  ★ CHEAP, MODEST
- **Size (measured, firefox):** 32 MB→512 MB = 6.17 s→**5.77 s (−6.5 %) AND
  RSS 1886→1847** (lower!). Non-monotonic: 128 MB is *worse* (6.63 s) — high
  hit-rate → frequent scavenge → survivor-copy cost. The win is in the
  **large/rare-collect** regime (nursery ≳ working set ⇒ objects die in place,
  ~no promotion/copy).
- **Lever:** raise the default nursery (workload-adaptive — sized to ~working
  set), OR make scavenge fire below `exitDepth==0` (riskier).
- **Risk:** LOW (tuning knob, byte-id). Tradeoff: RSS floor vs CPU; sweet spot is
  workload-dependent (firefox ~512 MB; M5 likely larger).
- **Falsifier:** min-of-5 firefox + the M5 size curve (7.3 % hit ⇒ more headroom)
  + RSS. Ship an adaptive default only if CPU win holds at acceptable RSS.

### L3 — cut unforced-thunk churn (62–67 %)  ★ STRATEGIC, HARD
- **Size:** the biggest structural prize — drives ALLOC (~20 % CPU) AND thunk
  memory (637 MB M5 = the RSS dominant). Halving churn ≈ −10 % CPU + −300 MB M5.
- **Lever:** deeper strictness/eagerness than the shipped call-unthunk pass
  (e.g. eager attrset-field/let-body eval when provably forced); or a cheaper
  never-forced-thunk representation.
- **Risk:** HIGH — eager eval can surface errors lazy eval wouldn't ⇒ byte-id /
  correctness hazard. Needs a conservative provably-forced analysis.
- **Falsifier:** measure the *provably-forced* fraction of OP_MAKE_THUNK sites
  statically before building anything (sizing gate, like FP-3 did).

### L4 — hoist macOS thread-local lookups  ★ NICHE
- **Size:** `_tlv_get_addr` ~6 % M5 / 3.6 % firefox (macOS-specific; Linux TLS is
  a register read). `threadArena()`/`activeVMStack()` are re-resolved per use.
- **Lever:** cache the TLS pointers in dispatchLoop locals across the hot loop.
- **Risk:** MEDIUM (TLS identity is stable within a VM dispatch; must not leak
  across VM boundaries). Falsifier: hoist threadArena into the loop, measure M5.

### L5 — allocator / std:: + memmove churn  ★ DIFFUSE
- Part of ALLOC ~20 %: Bindings-merge entry copying (`_platform_memmove`),
  value-stack `vector` growth, std::set/string in parse/lower. Diffuse; attack
  via L1/L2/L3 first (they remove the upstream allocations).

## What is NOT a lever (falsified / dead — do not revisit)
- forceValue / callClosure micro-opt (≤3 % combined at scale).
- AttrSelect shapes/PIC (<10 % hit — already killed, SP-1..4).
- Pair-tax / ValuePair shrink (closed 2026-06-20 — net-marginal at high risk).
- Computed-goto dispatch (neutral — CG-1..4).

## Recommended sequence
1. **L1 countDistinct** (best risk/reward; low-risk ~15 % on ff/HNE) — implement
   gated, byte-id, `--brute`, darwin-4 grade.
2. **L2 nursery size** (cheap; do the min-of-5 + M5 curve; ship adaptive default
   if it holds) — can run in parallel with L1's grading.
3. Add a **cache-ON** measurement row to separate eval-loop gap from lowering tax.
4. **L3 thunk churn** only after a static provably-forced sizing gate justifies
   the laziness risk.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
