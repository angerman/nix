# v3-autoresearch — research-org program

This is the `program.md` analog from Karpathy's autoresearch, tailored to the v3 bytecode
VM. The agent reads THIS (not the C++ directly) to know the objective, the hard
constraints, and the keep/revert bars. Edit this file to steer the loop — prefer editing
the spec over hand-editing the engine.

Design rationale + full context: `lode/AUTORESEARCH_V3_DESIGN_2026-06-16.md`.

## Objective (pick ONE per run; the loop optimizes this single target)

> Default: **lower v3 CPU on the `git` row** (a REAL nixpkgs drvPath eval) without
> regressing any other row.

**REAL-WORLD-GAINS RULE (2026-06-16, learned the hard way — see
do-not-repropose.tsv "foldl-microopts-realworld-neutral"):** the objective MUST be a
real workload row (`git` / `hello` / `firefox` / `M5`), NOT the synthetic `foldl`/`fib`
rows. A win on `foldl`/`fib` alone is presumed BENCHMARK-TUNING and does NOT count —
the synthetic FOLD is artificially dominated by one or two ops (e.g. non-rec
attrset-construction, small-int toString), so optimizing it can net ~0% on hello/git
(measured: the attrset-demotion + toString-cache levers gave ~18% on FOLD but ~1% / −0.8%
on hello/git). Every keep MUST improve a REAL row by the bar AND not regress the others.
Use `foldl`/`fib` only as fast directional proxies, never as the keep target.

Where the real CPU actually goes (git.drvPath sample, 2026-06-16): `forceValue →
callClosure → dispatchLoop` (~53%), driven by `primFoldl` + `primFilter` (nixpkgs
`lib.foldl'`/`filter`/`map`).  The real-world lever is **per-element callClosure /
dispatch-loop overhead**, not record-construction.  (Core-vm.cc → slow rebuild + higher
risk than the §"Where to look" sweet spot; treat as a careful manual-RCA target, or let
the loop attack it via opt-passes that cut dispatch on real lib iteration.)

Alternative objectives (swap the `OBJECTIVE_ROW`/`OBJECTIVE_METRIC` below):
- lower v3 arena (MB) on `firefox` / `M5` (deterministic metric — preferred for memory)
- lower v3 CPU on `hello` / `firefox` / `M5`

```
OBJECTIVE_ROW=git          # a REAL workload — never `foldl`/`fib` (benchmark-tuning)
OBJECTIVE_METRIC=cpu       # cpu | arena
```

## HARD constraints (a change that violates ANY is an automatic REVERT)

1. **Byte-identical to TW.** Every measured row's v3 result must equal the TW result. A
   `DIVERGENT` verdict is a REVERT, full stop — never a "win." (This is the gate's
   non-negotiable; a faster-but-wrong drvPath corrupts the store/cache.)
2. **v3 must be ENGAGED.** The v3 arm must show a `v3-direct` / `v3_arena` stats line. If
   it doesn't, the change broke v3-direct routing or we measured TW — ABORT, do not trust
   the number.
3. **No regression** on any of the 7 pinned rows beyond tolerance (CPU +3%, arena +5% vs
   `bench/baselines/seven-rows.tsv`). A win on the objective that regresses another row is
   a REVERT unless it nets out and is re-pinned with justification.
4. **lang suite stays green** (`--core` minimum; full 143 before any merge).
5. **No benchmark special-casing.** The change must not branch on the specific benchmark
   expression, attr name, or workload shape. The adversarial verifier checks this.
6. **No new env-gate without an inline retirement criterion** (repo rule 4). Prefer no new
   gate at all — the loop ships changes, it does not park them behind opt-in flags
   (Rule 0: optimization carcasses are forbidden).
7. **No edits in fenced-off areas** (§ below).

## Keep / revert bars (pre-committed; measure-twice)

- **KEEP-CANDIDATE** if the objective metric improves by **≥ 3%** vs the pinned baseline,
  byte-identical, engaged, no other-row regression. Promote to full 7-row gate + adversarial
  verify + (queue) darwin-4 flip-soak.
- **NEUTRAL** if |Δ| < 3% (within noise) → revert (avoid carcasses).
- **REVERT** if worse than baseline tolerance, DIVERGENT, or any hard constraint fails;
  append a one-line reason to `do-not-repropose.tsv`.
- **PROVISIONAL until broad sweep.** A KEEP-CANDIDATE is committed but NOT default-flipped
  for any GC/representation change until a periodic full nixpkgs flip-soak confirms it. The
  agent proposes; it does not self-flip a default (cf. gen-major rollout `e863f127d`).

## Where to look (high ROI, low risk, fast rebuild)

- `opt_*.cc` (optimizer passes) and `emit.cc` (lowering / superinstruction fusion) — small
  TUs, CPU-measurable, byte-identity-gated. Target dispatch-count reduction on foldl/list-iter.
- Continuous tuning knobs: nursery size, gen-major trigger thresholds, inline-cache sizes,
  growth factors. Arena MB is deterministic → tight keep/revert.

## FENCED OFF (do NOT edit autonomously — needs human RCA; an optimizer here re-introduces
## the masking / cross-write bug family)

- `derivationStrict` and anything touching drvPath / store-path semantics.
- The FFI boundary (`ffi.cc`, the TW-leaf calls).
- The writeback / cell machinery — `applyForceWriteback`, `CFF_FORCE_*`, `cellWrite`,
  the SELECT writeback sites (the C-1 family).
- Error-handling that could mask a failure as a value (the fake-store class).

## Read before proposing

- `do-not-repropose.tsv` — already-falsified levers; do not re-propose them.
- `bench/baselines/seven-rows.tsv` — current pinned baseline (the comparison).
- `lode/PLAN_BEAT_TW_V2_2026-06-13.md` §do-not-repropose — the full kill list with evidence.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0.*
