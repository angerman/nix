# DEFECT_AUDIT §9 execution — session arc (2026-07-02)

One-session index of the work that executed the `DEFECT_AUDIT_2026-07-02.md` §9
priority matrix. Read the audit + its embedded WS-0..WS-6 handbacks for detail;
this is the scannable arc + the verdict per item. HEAD at close: `1b7f35ae9`.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.

## Bottom line

Every §9 item was driven to a **measured decision**. All cheap/safe wins
shipped; **five** audit proposals were **falsified/measured-NO-GO** (kills are
deliverables): P0.2, P3.3, P4.4-CPU, P2.3-inherit, and **P3.1** (chain-aware read
IC — built + adversarially-validated + brute-28/28, then a depth counter proved
chains are shallow (avg 2.44) so the IC is a structural wash → measured **0% CPU**
→ **DELETED** per Rule 0, design preserved at `0cb2f88b5`). The one real latent
bug was investigated + resolved. The remaining lever (P2.1-a) is a
genuine W-scale, drv-hash-critical build — fully designed + de-risked. This
matches the audit's own §9 honest expectation: **no single remaining CHEAP lever
moves the gap** — even the §3.2 "headline per-lookup tax" yields 0% once built
and measured, because real-nixpkgs chains are shallow.

## Arc (oldest → newest)

| Commit | Item | Verdict |
|---|---|---|
| `d6729f753` | P0.1a gate per-primop counter | shipped (SIGBUS lesson: no inline field on `static const PrimOp`) |
| `016bef7f0` | P0.1b mergeBindings counters → V3_STATS | shipped |
| `2b565401d` | P0.1c selector/intrinsic/keepPap counters | shipped |
| `2d7efdb97` | P0.3 decouple limit-poll from kAnySlowGate | shipped; falsifier PASS |
| `b39e99fec` | WS-0 handback + authoritative re-baseline | done |
| `c3657c94b` | P1.1 branch opcodes reject non-Bool | shipped (TW-parity + fixtures) |
| `c9948463f` | P1.2 primSort Phase-D barrier | shipped (PhD-6 UAF; failing-first repro) |
| `5235df5e9` | P1.3 disk-hit catch scope | shipped (import-error propagates) |
| `d439a0de7` | WS-1 handback | done (P1.4 closed: mechanism real, M5-unavailable) |
| `09bb48f51` | P2.1 step-0 measure | **GO** — formal wrappers 12.79% of thunk allocs, 47% never forced |
| `a7948a2f3` | P2.1 design-b | **FALSIFIED** — bind-at-entry → module-system recursion (deferral load-bearing) |
| `4a7055a22` | P3.4 valueEqual scalar fast path | shipped |
| `adb81c18d` | darwin-4 WS-1+P3.4 validation | cost-free + small win |
| `14d8e7e25` | P3.5a barrier hint flip | shipped |
| `b534235d5` | P3.2 formals-call needsForce guard | shipped |
| `c27d12f09` | §3.14 OP_ATTRS_SELECT dead-chase removal | shipped (BI-neutral) |
| `803b0af11` | §3.14 SELECT_DYN hoist + emit dead dup-loop | shipped (BI-neutral) |
| `152c6605f` | **P3.3 ATTRS_INIT emit-sort** | **FALSIFIED + reverted** — runtime sort is load-bearing for the cross-process cache; adversarial review caught a warm-cache corruption; added `run-attrs-init-cache-roundtrip-tests.sh` guardrail |
| `362f87be9` | P3.5 constexpr phaseDActive + P3.8 std::move | shipped (BI-neutral) |
| `995983bcf` | WS-3 darwin-4 validation + **P4.4 CPU falsifier** | batch **CPU-NEUTRAL**; P4.4 string-context re-parse memo = −0.4% → **CLOSE (unfunded for CPU)** |
| `8cbb07cec` | WS-3 handback + dispositions | done |
| `efa758a42` | WS-2/4/5/6 handback | done (honest per-item dispositions) |
| `6ebdbd0db` | **#14 missed-root** investigation | **RESOLVED** — known RCA'd latent, default-path barrier-complete, unreproducible (56+ stressed evals), EVAC-path already fixed |
| `143120272` | P2.3 step-0 measure | **SPLIT** — or-default 3.66% GO-but-sub-noise; inherit-in-rec 0.05% **CLOSE** |
| `1b7f35ae9` | P2.1-a design refined (bytecode evidence) | reframed: a cycle-safety-analysis extension, not a greenfield opcode |

## Falsifications (Rule 0 kills — the deliverables)

1. **P0.2** `-Dv3_release=true`: within noise → keep `false`. The only material
   instrumentation was the primop counter (fixed by P0.1a).
2. **P3.3** ATTRS_INIT emit-time sort: the runtime sort is **load-bearing** —
   OP_ATTRS_INIT pushes values positionally, and cross-process `remapAllSymbols`
   permutes SymbolIds but can't reorder the value pushes (only slot-indexed
   REC_INIT survives). Adversarial review found a warm-cache corruption in the
   flag-bit attempt. Reverted + guardrail suite added.
3. **P4.4-CPU** string-context re-parse: `V3_DBG_CTX_PARSE_MEMO=1` on
   firefox.drvPath = −0.4% (< 3% bar) → the cost is in the copies, not the
   parse-back; lever 1.1 unfunded for CPU.
4. **P2.3 inherit-in-rec** (0.05%) + **P1.4** (huge-block UAF, unreproducible).

## darwin-4 numbers (git-noted; `362f87be9`)

- WS-3 batch **CPU-NEUTRAL**: firefox cold 2.68→2.67s, warm 1.80→1.79s vs P0.4.
  Every shipped micro-op is individually noise-level — WS-1's value is
  correctness, not CPU.

## Key lessons

- **Adversarial verification earns its keep**: the P3.3 warm-cache corruption
  (four serialize walkers reading the operand as a raw count) was caught
  pre-merge by an independent refuter, not the brute (which had a warm-cache
  coverage gap — now closed).
- **Header changes need `ninja -C build` for ALL test binaries** — rebuilding
  only v3-eval+nix left a stale v3-smoke referencing a removed symbol (dyld
  fail, caught by the brute).
- **"allocated ≠ realized" (FP-2) recurs**: P2.3 or-default clears the 2%
  alloc-share proxy yet its realized CPU is sub-noise. Always convert
  alloc-share GOs to a realized-CPU estimate before funding a build.
- **Overloading a bytecode operand with a flag bit is a footgun** — every
  generic trailer walker (VM, serialize×4, comparator, disasm) must mask it.
- **Never rush a falsified-naive, not-fully-RCA'd, BI-critical change**: P2.1-a's
  naive form (design-b) recursed for reasons still not fully understood
  (param is force-validated before entry), so a variant can't be built safely
  until design-b's recursion is RCA'd.

## P3.1 chain-aware read IC — BUILT this session (user "continue!"), gated, measured NO-GO on measurable workloads

After the user chose the boundary then said "continue!", P3.1 was built the safe
way and measured:
- `0cb2f88b5` — chain-aware read IC (`NIX_V3_CHAIN_IC`, default-off): caches the
  resolved `(chainLeaf, ownerLayer, slot)` in the shared per-call-site
  `attrSelectCache` (extended `AttrSelectIC::Entry` with `ownerLayer`); the 5
  gc.cc IC scavenge/audit sites gray/visit `ownerLayer` (Bindings are always
  tenured → safe); both IC-clear sites + the flat install null `ownerLayer` (no
  stale gray). Adversarial review NOT-REFUTED (5 angles); `--brute` 28/28 gate-ON
  (moving-GC missed-root + drv byte-id) AND 28/28 default; ON-vs-OFF byte-id on 5
  chain-SELECT exprs incl. a 20-layer `foldl //` chain.
- `30269d3ab` — darwin-4 A/B (git-noted): firefox.drvPath **0%** (2.66s==2.66s),
  git.drvPath **0%** (1.41s==1.41s). No measurable CPU win.
- **A chain-SELECT depth/hit-rate counter settled WHY (the decisive data):**
  firefox — chain-SELECTs are **60.4 %** of all selects (COMMON, not rare), avg
  chain depth **2.44 layers** (SHALLOW), IC hit-rate **45.9 %**; git — 57.5 %,
  depth 2.35, 44.9 %. So the IC fires + hits ~45 %, but each hit saves only a
  ~2.4-hop walk over small overlays ≈ the IC's own 4-way scan+validate → a
  **structural wash → 0 % CPU**. §3.2's "walks all ≤16 layers" is refuted
  (`mergeBindings` flattens every 16 + SELECTs short-circuit at the top overlay →
  effective depth ~2.4 even though chains dominate).
- **DELETED per Rule 0** (a gate with no proven win must retire, not coexist):
  code restored to pre-P3.1; the validated implementation is preserved at
  `0cb2f88b5` for a trivial re-apply IF a future x86_64 run first measures M5 avg
  chain depth ≫ 4 (unlikely, given the flatten-cap + short-circuit). Third
  measure-first "the §-headline lever doesn't materialize" result (cf. P3.3,
  P4.4-CPU) — the deliverable is the design + the shallow-chain finding.

## Remaining levers (dedicated next-session efforts)

Fully designed + de-risked here:

- **P3.1 flip decision** — needs an x86_64 M5 A/B + hit-rate counter (above).
- **P2.1-a — formals wrapper elimination** (12.79% thunk allocs, ~2.5% CPU —
  the one micro-lever plausibly above darwin-4 noise). NOT a greenfield opcode:
  v3 already inlines formal selects at entry when cycle-safe
  (`{a,b?5}:a+b` emits no wrapper); the residual wrappers are the cases the
  strictness/cycle-safety inliner (`opt_func_strictness` +
  `opt_strict_call_unthunk`) conservatively skips ("formals-style would need
  attrset-entry-level rewriting"). **Prerequisite: RCA design-b's recursion**
  (reproduce gated + instrument), then extend the analysis to formals, gated +
  drv-hash byte-identity validated across nixpkgs.

The broader RSS program (P4.1/4.2/4.3/4.7 — leaner live representation), P6
(compile-time), and JIT remain the multi-week structural work the beat-tw
campaign already characterized.
