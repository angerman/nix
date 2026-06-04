# Static bytecode opcode + n-gram analysis — register-VM / superinstruction decision input

**Date:** 2026-06-04
**Status:** MEASUREMENT + DECISION INPUT. Static opcode-frequency and
bi/tri/4-gram analysis of the compiled v3 bytecode across three real
workloads, to inform whether a register-VM rework and/or targeted
superinstructions are worth building (the largest open wall-time lever
post Stage-5/6 kill). Includes the tooling, the data, the verdict, and a
measure-first `/goal` (§7).
**Author:** session synthesis (code-grounded; tooling at `emit.cc` +
`bench/analyze-bytecode.py`, validated 0 decode errors over 13.2M insns).

Companion docs:
- [`OBSERVABILITY_AUDIT_2026-06-03.md`](OBSERVABILITY_AUDIT_2026-06-03.md) — the measurement-surface audit this extends
- [`POST_PURE_PIPELINE_OPTS_2026-06-02.md`](POST_PURE_PIPELINE_OPTS_2026-06-02.md) §6 — "dispatch ~5% of wall" (the upside bound)
- [`V3_VM_STATE_2026-06-02.md`](V3_VM_STATE_2026-06-02.md) §5.7 — register-VM listed as the biggest post-5/6 wall lever; #778 (48.9% dispatch) / #780 / #782 priors

---

## 1. The tooling (what was built)

- **`emit.cc::compile`** — env-gated `NIX_V3_EMIT_BYTECODE=1`
  (+ `NIX_V3_EMIT_BYTECODE_OUT=<path>`) dumps a full disassembly of EVERY
  compiled CU (top-level + each imported module — `compile()` is the single
  chokepoint). Reuses `disasm.cc::opExtraWords`, so the **variable-width**
  stream (opcode + 24-bit operand + opcode-specific trailing data words) is
  advanced correctly. Retirement criterion in the inline comment (supersede
  with a `--emit-bytecode` CLI flag).
- **`bench/analyze-bytecode.py`** — parses the dump → opcode histogram +
  bi/tri/4-grams (broken at function boundaries OP_RETURN/OP_HALT) +
  superinstruction-candidate ranking by dispatch savings `(len-1)·count`.

**Decode validation:** 0 `<unknown>` lines across all three dumps
(13.2M instructions) ⇒ the width logic decoded the entire corpus with zero
misalignment.

### Reproduction (forces full recompile for complete static coverage)
```bash
# hello.drvPath (nixpkgs/stdenv family)
NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1 \
NIX_V3_EMIT_BYTECODE=1 NIX_V3_EMIT_BYTECODE_OUT=/tmp/bc-hello.txt \
NIX_V3_MAX_WALL_TIME=120s NIX_V3_MAX_HEAP=3G \
  ./build/src/nix/nix eval --impure --expr '(import <nixpkgs> {}).hello.drvPath'

# HNE  (haskell.nix family)
… --expr '(builtins.getFlake "/Users/angerman/Projects/iohk/haskell-nix-example").packages.x86_64-linux.hello.drvPath'
# M5   (cardano-node — built ON haskell.nix)
… NIX_V3_MAX_HEAP=6G --expr '(builtins.getFlake "/Users/angerman/Projects/iohk/cardano-node").outputs.packages.aarch64-darwin.cardano-node.name'

python3 src/libexpr-v3/bench/analyze-bytecode.py /tmp/bc-<wl>.txt --top 20 --max-n 4
```
(Use the `nix` binary — `v3-eval` does not resolve flake-`<nixpkgs>`; the
`nix eval` driver compiles via the same v3 path so the hook still fires.)

---

## 2. The data (2026-06-04)

| | hello.drvPath | HNE | M5 (cardano-node) |
|---|---|---|---|
| CUs | 284 | 2,467 | 2,852 |
| instructions | 1,025,506 | 5,876,830 | 6,318,259 |
| **stack-motion %** (GET/SET_LOCAL + GET_UPVALUE + FORCE + fused) | **50.1%** | **49.6%** | **49.3%** |
| top-16 bigrams (share of all bigrams) | 69.4% | 77.0% | 75.3% |
| top-16 trigrams | ~60% | 62.0% | 60.5% |
| decode errors | 0 | 0 | 0 |

**Static (49–50%) matches the documented dynamic 48.9% (#778)** — three
static workloads + the dynamic counter all agree stack motion is ~half of
all dispatch.

Top opcodes (all three): `OP_GET_LOCAL` ~19%, `OP_SET_LOCAL` ~15%,
`OP_GET_UPVALUE` ~12%, then `OP_RETURN` / `OP_ATTRS_REC_SET` /
`OP_MAKE_THUNK` ~8–9% each.

---

## 3. Two families, one shape

**M5 ≈ HNE because cardano-node is built ON haskell.nix** — they share
eval infrastructure and are NOT independent samples. There are two families:

- **nixpkgs/stdenv** (hello): dominated by rec-attrset fill —
  `GET_LOCAL ATTRS_REC_SET` (8.7%), `GET_UPVALUE MAKE_THUNK`.
- **haskell.nix** (HNE≈M5, ~6× larger): dominated by let-rec slot traffic —
  `GET_UPVALUE REC_BINDING_SLOT_REF SET_LOCAL` (top trigram ~5.8%) — plus the
  **selector-thunk** `GET_UPVALUE_FORCE ATTRS_SELECT RETURN` (~223K, ~4.3%).

Both agree on the headline (≈49–50% stack motion, 69–77% top-16 bigram
concentration). **Common denominator across both families:** rec-attrset
fill (`GET_LOCAL ATTRS_REC_SET` alternating) + pure stack-motion runs
(`GET_LOCAL`×3–4).

---

## 4. Superinstruction candidates (cross-workload, ranked by stability)

| # | Sequence | Where | Static occ | Capture |
|---|---|---|---|---|
| 1 | `GET_LOCAL ATTRS_REC_SET` (alternating) | **all 3** (#1) | 81K hello / 288–451K hne+m5 | fused `ATTRS_REC_SET_FROM_LOCAL` (write `entries[i]` from slot, skip push) |
| 2 | `GET_UPVALUE REC_BINDING_SLOT_REF SET_LOCAL` | haskell.nix | 292K / 302K | fold upvalue-load into the slot-ref consumer |
| 3 | `GET_UPVALUE_FORCE ATTRS_SELECT RETURN` | haskell.nix | 221K / 223K | **selector-thunk** (= the killed Stage 7 pattern, but pure superinstruction, independent of Stages 5/6) |
| 4 | `GET_LOCAL`×3–4 (pure run) | haskell.nix | 174–215K | register-VM target (no consumer to fuse into) |
| 5 | `GET_UPVALUE MAKE_THUNK SET_LOCAL` | all | 57K–172K | fused thunk-bind |

Savings caveat: candidates OVERLAP (one `GET_LOCAL` counts in its bigram,
trigram and 4-gram), so per-row savings are individual ceilings, not
additive.

---

## 5. Verdict — register-VM is the general lever; the data favors it

The dominant n-grams are consistently **"push operand → consumer"** pairs
(`GET_LOCAL→ATTRS_REC_SET`, `GET_UPVALUE→REC_BINDING_SLOT_REF / MAKE_THUNK /
ATTRS_SELECT`). That shape is decisive:

- A **register VM** that lets those consumers read operands directly from
  slots eliminates the push — collapsing both the top patterns AND the ~49%
  stack motion structurally. The n-grams confirm the pushes are *paired with
  consumers*, not free-floating, which is exactly what makes operand-folding
  (register VM) the right general lever.
- **Targeted superinstructions** (candidates 1–3, 5) are the cheaper
  incremental capture of the same patterns; candidates 1 and 3 are
  cross-family-stable and the safest single bets.

By the team's own pre-committed **#780 criterion** ("top-20 bigrams < 30% →
stack motion is spread, not pair-fusible"), all three workloads clear the
bar by **2–2.5×** (69–77%) → fusion/register-VM is justified, not
falsified, and stable across both families.

---

## 6. Honest limits (load-bearing for the goal)

- **Upside is bounded.** `POST_PURE_PIPELINE_OPTS §6` puts dispatch at ~5%
  of wall. Register-VM removes more than pure dispatch (it removes the
  push/copy/force WORK of the stack-motion ops — why `OP_GET_LOCAL_FORCE`
  was added), but the win is bounded and the interpreter ceiling (~1.5–2×
  native) is unchanged. **This is the load-bearing unknown: the pattern is
  fusible (proven), but whether capturing it moves WALL meaningfully is
  unmeasured.**
- **Static ≠ dynamic.** Static over-weights cold code. Static agrees with
  the dynamic *opcode* histogram here, but **dynamic n-grams** (execution-
  weighted) are the decision-grade signal and don't exist yet (the counter
  is bigram-only, #782). A hot-loop trigram can dwarf its static count.
- **n-grams are linear**, broken at function boundaries; branches not
  followed (the static-adjacency approximation an emit-time fuser keys on).
- **HNE and M5 are one family** (haskell.nix), not two independent points.

---

## 7. `/goal` — measure-first, then capture the lever

**PRIMARY GOAL: reduce v3 dispatch wall-cost by capturing the ~49%
stack-motion traffic — but PROVE the wall payoff on a cheap spike BEFORE
committing to the multi-week register-VM rework.**

Per [[measure-twice-cut-once]] (the "no cheap proxy → cap the spike +
pre-commit keep/revert" variant), the static + documented-dynamic data
establishes the pattern is fusible; it does NOT establish the wall payoff.
So:

**Step 1 — execution-weighted confirmation (~1–2 d).** Extend the existing
`NIX_VM_BIGRAMS` counter (alloc.hh `bigramCounts`, vm.cc:3045) to **trigrams**
via a sparse map (dense `[256]³`=128 MB is too big), gated, hot-path-cheap.
Run on hello + HNE/M5. **Pre-commit:** proceed only if the dynamic top-10
trigrams overlap the static candidates (§4) AND top-20 dynamic bigrams ≥ 30%
(the #780 bar) on a real workload.

**Step 2 — bounded superinstruction spike (≤2 d / ≤200 LoC).** Implement the
ONE cross-family-stable candidate with no register-VM rework: **candidate 1**
(`ATTRS_REC_SET_FROM_LOCAL`) OR **candidate 3** (selector-thunk
`GET_UPVALUE_FORCE ATTRS_SELECT RETURN`). Emit-time peephole in emit.cc +
one dispatch arm in vm.cc. No opt-in gate during measurement.
**Pre-commit BOTH:**
- KEEP if hyperfine (≥10 runs) shows ≥ 3% wall reduction on hello.drvPath OR
  HNE, drvPath byte-identical, `--quick` 6/6 + `--core` 19/19.
- REVERT (commit the data + threshold + rationale) if < 1%.

**Step 3 — decision gate.** If the single-superinstruction spike clears 3%,
the full **register-VM / operand-folding rework** is justified (it
generalises the win across all push→consumer pairs); scope it as its own
multi-week arc with a fresh SHIP gate. If the spike falls below 1%, the
dispatch lever is below the wall noise floor on these workloads —
**falsify it** (the pattern is fusible but the wall doesn't care; memory /
the GC remains the higher-slope axis) and write the Rule-0 doc.

**Anti-spiral contract:** the static n-grams are NOT a license to build the
register VM directly — Step 2's cheap spike is the gate. Three failed
superinstruction spikes on the same premise = falsification of the dispatch
lever.

---

## 8. Step 1 RESULTS (2026-06-04) — dynamic confirmation: **GATE PASSED**

The trigram counter is built (sparse `std::unordered_map<uint32_t,uint64_t>`
in `AllocStats`, packed 24-bit `(pp<<16)|(p<<8)|c` key; gated
`NIX_VM_TRIGRAMS=1` alongside `NIX_VM_OPCOUNTS=1`; rolling 0xFF-sentinel
state in the `vm.cc` dispatch block; top-20 dumped from `run.cc` (production
`nix eval` path) and `cli/v3-eval.cc`). Run on all three workloads via
`nix eval` (the doc §1 reproduction, plus `NIX_VM_STATS=1`).

| | hello.drvPath | HNE | M5 |
|---|---|---|---|
| dynamic dispatches | 16.25 M | 94.93 M | 679.92 M |
| **bigram top-20** (the gate) | **58.67 %** | **58.51 %** | **57.48 %** |
| trigram top-20 (distinct triples) | 35.12 % (2823) | 34.56 % (3572) | 34.25 % (3642) |
| top bigram `SET_LOCAL→GET_LOCAL` | 7.69 % | 7.77 % | 8.15 % |
| top trigram `SET_LOCAL GET_LOCAL GET_LOCAL` | 4.60 % | 4.63 % | 4.56 % |

**Note (static ≠ dynamic scale):** dynamic dispatch is 15–108× the static
instruction count (cold code under-weighted statically; hot loops
re-execute). Despite that, the *shape* is even MORE stable dynamically than
statically — bigram top-20 sits at 57–59 % across all three, top bigram at
~7.7–8.2 %, top trigram at ~4.6 %.

**Pre-committed gate (§7 Step 1): proceed only if dynamic top-10 trigrams
OVERLAP static §4 candidates AND bigram top-20 ≥ 30%.**
- Bigram top-20 ≥ 30%: **PASS by ~2×** (57–59 % on all three).
- Top-10 overlap: **PASS** — see candidate verification below.

### Candidate verification (the value Step 1 added over static)

- **Static #1** rec-attrset fill (`GET_LOCAL ATTRS_REC_SET` alternating):
  **CONFIRMED dynamically hot.** hello dynamic top-20 has
  `ATTRS_REC_SET GET_LOCAL ATTRS_REC_SET`, `GET_LOCAL ATTRS_REC_SET GET_LOCAL`,
  and `GET_LOCAL MAKE_THUNK ATTRS_REC_SET`; HNE/M5 carry
  `GET_LOCAL MAKE_THUNK ATTRS_REC_SET` (1.4–1.6 %).
- **Static #2** `GET_UPVALUE REC_BINDING_SLOT_REF SET_LOCAL`: **CONFIRMED —
  dynamic #2 on ALL THREE** (2.39–2.82 %). The single most stable
  cross-family triple.
- **Static #3** selector-thunk `GET_UPVALUE_FORCE ATTRS_SELECT RETURN`
  (static ranked ~4.3 % on HNE): **FALSIFIED as a dynamic hotspot — NOT in
  the dynamic top-20 of ANY workload.** Pure static artifact: many distinct
  selector thunks are *compiled* (high static count) but each *executes* few
  times. **⇒ candidate #3 is OUT for Step 2.**
- **Static #5** `GET_UPVALUE MAKE_THUNK SET_LOCAL`: **CONFIRMED** —
  dynamic #7 (hello, HNE), #18 (M5).
- **NEW dynamic-only top triple** not flagged statically:
  `SET_LOCAL GET_LOCAL GET_LOCAL` (#1 on all three, 4.6 %) — "write a slot,
  read it twice". Generalises the top bigram `SET_LOCAL→GET_LOCAL` (7.7–8.2 %)
  and is a prime operand-fold / register-VM target.

### Step-2 decision (refined by the dynamic data)

The §7 menu offered candidate **#1** (`ATTRS_REC_SET_FROM_LOCAL`) **OR #3**
(selector-thunk). Step 1 **falsifies #3** (static-only) and **confirms #1**
(dynamically hot, cross-family). **Step 2 implements candidate #1.** The
`SET_LOCAL→GET_LOCAL` family (top bigram + top trigram) is the obvious
register-VM target should Step 2 clear the 3 % gate, but it is NOT a clean
emit-time peephole (write/read are separated by arbitrary intervening ops),
so it stays a Step-3 register-VM concern, not a Step-2 spike.

### Build hazard observed

Adding a member to `AllocStats` (a widely-included header type) requires
rebuilding EVERY binary that includes `alloc.hh` — `v3-eval`, **`v3-smoke`**,
and `nix` — not just `libnixexprv3.dylib`. A stale `v3-smoke` linked against
the rebuilt dylib hit an ODR layout mismatch on the shared `inline
allocStats()` singleton and failed the quick suite until rebuilt (then 9/9
GREEN). `ninja -C build src/libexpr-v3/{v3-eval,v3-smoke} src/nix/nix`.

### Reproduction
```bash
NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1 \
NIX_VM_STATS=1 NIX_VM_OPCOUNTS=1 NIX_VM_BIGRAMS=1 NIX_VM_TRIGRAMS=1 \
NIX_V3_MAX_WALL_TIME=900s NIX_V3_MAX_HEAP=6G \
  ./build/src/nix/nix eval --impure --expr '<workload>' 2>/tmp/tg.txt
# final cumulative dump = the largest total (run.cc dumps per runRootExpr):
grep -oE 'trigrams: total=[0-9]+' /tmp/tg.txt | sort -t= -k2 -n | tail -1
```

---

## 9. Cross-references
- [[observability-audit-2026-06-03]] — measurement surface (this extends it)
- [[post-pure-pipeline-opts-2026-06-02]] §6 — dispatch ~5% of wall (the bound)
- [[v3-vm-state-2026-06-02]] §5.7 — register-VM as biggest post-5/6 lever; #778/#780/#782
- [[measure-twice-cut-once]] — Step 2's cap+pre-commit variant
- [[falsification-rule]] — Step 3 exit
- Code: `emit.cc::compile` (NIX_V3_EMIT_BYTECODE), `bench/analyze-bytecode.py`,
  `disasm.cc::opExtraWords` (the width table), `alloc.hh::bigramCounts` +
  `alloc.hh::trigramCounts` (Step-1, built 2026-06-04) + `vm.cc` dispatch
  block (NIX_VM_TRIGRAMS) + dumps in `run.cc` / `cli/v3-eval.cc`

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
