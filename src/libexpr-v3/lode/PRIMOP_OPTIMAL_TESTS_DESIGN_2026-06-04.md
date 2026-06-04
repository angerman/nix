# Testing that native primops compile to optimal bytecode: FileCheck vs allocation budgets

**Date:** 2026-06-04
**Status:** DESIGN — **validated** by a worked `foldl'` experiment (§1). Answers
"can we FileCheck all our native primops to ensure optimal bytecode?" The short
answer: FileCheck yes, but at the per-*optimization* granularity (already in use);
per-*primop* "optimal" is robustly guarded by **per-element allocation/instruction
budgets**, not full-body FileCheck.
**Triggering question:** "Could we design tests for all our native (nix) primops
(like `foldl'`) with FileCheck to ensure we compile to the optimal bytecode?"
**Author:** session synthesis

Companion docs:
- [`EVAL_OPT_ROADMAP_2026-06-04.md`](EVAL_OPT_ROADMAP_2026-06-04.md) — the optimizations these tests guard/track
- [`QUADRATIC_BUILD_AND_FUSION_SURVEY_2026-06-04.md`](QUADRATIC_BUILD_AND_FUSION_SURVEY_2026-06-04.md) / [`BENCH_V3_VS_TW_2026-06-04.md`](BENCH_V3_VS_TW_2026-06-04.md) — the per-element cost framing
- `test/ir-fixtures/README.md` — the existing FileCheck infra (`v3-check`, `run-ir-checks.sh`)

---

## 0. TL;DR (validated)

* **FileCheck is the right tool — at the per-OPTIMIZATION granularity.** The team
  already does this: `letrecDemote-nonrec-pos.nix`, `seqForce-pos.nix`,
  `streamFusion-foldlMap-pos.nix`, etc. — one minimal-trigger fixture per pass, with a
  *clean* keyword signal. **Keep this; add one per roadmap optimization.**
* **Per-PRIMOP, full-body FileCheck is CONFOUNDED — do NOT rely on it.** Validated:
  toggling the let-rec-demotion gate produces a **clean 0↔1 `LetRec` delta on the
  minimal trigger** but **NO IR delta on `foldl'`'s full body** (the body legitimately
  contains the recursive `go`-LetRec, and the per-element effect is a subtle
  representation change masked by var-renumbering + other passes). A full-body
  FileCheck guard is a *tautology* — it passes whether or not the optimization fired.
* **The robust per-PRIMOP "optimal" guard is per-element ALLOCATION / INSTRUCTION
  BUDGETS:** apply the primop to a list of size N, read `NIX_VM_STATS`, assert
  `attrsets/elem`, `closures/elem`, `thunks/elem`, `insns/elem` ≤ a budget. Validated:
  `foldl'` over 200k elems is `attrsets 200001 → 1` ON↔OFF — a **clean, unambiguous
  delta** the full-body IR could not show. Bonus: it tests the **real installed
  primops** (no source-copy drift) and is the natural living spec.

---

## 1. Why full-body FileCheck doesn't work (the experiment)

Worked example: a fixture whose body was `foldl'`'s installed source, with
`# CHECK: LetRec / # CHECK-NOT: LetRec` (scoped "exactly one LetRec") + `# CHECK-NOT:
LitPrimOp "seq"`. It **passed** — but the negative control (run with the optimization
gates OFF) **also passed**, i.e. it guarded nothing.

| Signal | minimal trigger (`a:b: let x=a+b; in x+x`) | `foldl'` full body |
|---|---|---|
| `LetRec` count, demote ON | 0 | 1 (the recursive `go`) |
| `LetRec` count, demote OFF | **1** | **1** (no delta) |
| `LitPrimOp "seq"`, seqForce ON/OFF | clean delta on its own trigger | **0 / 0** (no delta) |
| applied-primop `attrsets`, demote ON/OFF | — | **1 / 200001** (clean delta) |

`v3-check`'s `CHECK-NOT` is fine (a synthetic 2-`LetRec` input fails correctly) — the
problem is that **a full primop body doesn't expose a clean keyword delta** for a
per-element optimization. The minimal trigger does; the applied-primop allocation
count does; the full-body IR does not.

**Root cause:** "optimal" for a primop is a *per-element cost* property, and the IR of
the whole body is the wrong altitude to read it — the construct legitimately appears
(recursive `go`), and the win is a subtle node-shape change, not a keyword
appearing/disappearing. Allocation counts read the cost directly.

---

## 2. The design (two complementary tools, distinct roles)

### 2a. Per-optimization FileCheck fixtures (minimal triggers) — KEEP + extend

The existing `test/ir-fixtures/` model. For **each optimization** in the roadmap, a
minimal-trigger `-pos` fixture asserting the clean post-opt signal (+ a `-neg`/gate
companion for the un-optimized form). These catch **a PASS regressing**:

| Optimization | Fixture (exists / to add) | Clean signal |
|---|---|---|
| non-rec-let demotion | `letrecDemote-nonrec-pos.nix` ✓ | `CHECK-NOT: LetRec` on `a:b: let x=a+b; in x+x` |
| seq → Force | `seqForce-pos.nix` ✓ | `CHECK-NOT: LitPrimOp "seq"` |
| foldl'∘map fusion | `streamFusion-foldlMap-pos.nix` ✓ | `CHECK: LitPrimOp "__foldlMap"` |
| **operator opcode-lowering (T3.A)** | *add* `opLessOpcode-pos.nix` | `CHECK-NOT: PrimOpCall "__lessThan"` on `a:b: a < b` |
| **strictness-through-recursion (#2)** | *add* `strictRec-pos.nix` | `CHECK-NOT: MkThunk` on the strict recursive arg |
| **build-fusion / transient builder** | *add* | the builder shape on `foldl' (acc:x: acc ++ [f x]) []` |

Rule: every optimization that lands ships its minimal-trigger fixture in the same
commit (this is already the team's practice — `cd1da2577` shipped `letrecDemote`).

### 2b. Per-primop allocation/instruction BUDGETS — the robust "optimal" guard

A new harness `test/run-primop-budgets.sh`: a table of
`{primop, applied-expr, N, budget{attrsets,closures,thunks,insns}-per-elem}`. For each
native primop it evals the applied expr under `NIX_VM_STATS` (+ `NIX_VM_OPCOUNTS` for
insns), computes per-element counts, and FAILs if any exceeds budget. Example rows:

```
foldl'   "foldl' (a:b:a+b) 0 (genList (x:x) N)"            N=100k  attrsets/elem<=0  thunks/elem<=K  insns/elem<=B
map      "length (map (x:x+1) (genList (x:x) N))"          N=100k  ...
filter   "length (filter (x: x>0) (genList (x:x) N))"      N=100k  ...
genericClosure / zipAttrsWith / sort / concatMap / ...     ...
```

Properties (all validated or argued above):
- **Robust:** reads the cost directly; immune to cosmetic IR/bytecode changes.
- **No drift:** exercises the *real* installed `builtins.foldl'` etc., not a copied
  source string.
- **Living spec / progress tracker (the no-XFAIL answer):** budgets are set to
  *current achievable*; a landed optimization **tightens** the relevant rows. Put
  not-yet-achievable (aspirational) budgets in a separate non-blocking target so they
  track the roadmap without breaking CI. (`v3-check` has no `XFAIL`, so budgets — not
  FileCheck — are the living spec.)

---

## 3. Coverage — the 13 native primops

`foldl'`, `__foldlMap`, `map`, `filter`, `all`, `any`, `concatMap`, `partition`,
`groupBy`, `sort`, `genericClosure`, `zipAttrsWith`, and the `derivationStrict` /
`derivation` hybrids — each gets a budget row (§2b). The roadmap optimizations each
get a per-opt fixture (§2a). `foldl'` today (post let-rec-demotion + seqForce): per
100k elems ≈ `attrsets 1` (✓ optimal), `thunks/elem`+`closures/elem`+`insns/elem ~94`
(the pending #2/#3/T3.A targets — the budget tightens as each lands).

---

## 4. The combined regime

Landing optimization **X**:
1. **Add** a per-opt minimal-trigger FileCheck fixture (§2a) — regression guard for the pass.
2. **Tighten** the affected primops' budget rows (§2b) — regression guard for the real cost.

The two are orthogonal and both necessary: FileCheck proves *the pass fires on a clean
trigger*; the budget proves *the real primop got cheaper and stays cheap*. The §1
experiment is exactly why you can't collapse them into one full-body FileCheck.

---

## 5. Honest limits / validated lessons

- **Full-body FileCheck is confounded** (§1) — the central finding; don't author
  per-primop fixtures that assert whole-body shape.
- **Budgets require the primop APPLIED** — an unapplied lambda body (`op: nul: list:
  …`) allocates nothing; the harness must apply to a real list of size N.
- **Budgets are workload-shaped** — per-element on a uniform list is the cleanest
  signal; document `N` + the exact expr per row (a pathological/heterogeneous list is
  a separate, optional row).
- **Optional `--emit-primop-ir <name>`** (dump an installed primop's IR by name) would
  let you snapshot per-primop IR drift-free — but per §1 it's for *inspection*, not a
  reliable pass/fail guard. Budgets are the guard.

---

## 6. Cross-references

- [[eval-opt-roadmap-2026-06-04]] — the optimizations §2a fixtures + §2b budgets guard
- `test/ir-fixtures/README.md` + `test/run-ir-checks.sh` — the per-opt FileCheck infra to extend
- `letrecDemote-nonrec-pos.nix` / `seqForce-pos.nix` — the per-opt model (the right granularity)
- [[measure-twice-cut-once]] — the §1 experiment is why this design is validated, not assumed
- [[falsification-rule]] — the first-cut "FileCheck per primop" hypothesis was *falsified* by §1; budgets replace it

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
