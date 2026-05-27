# `opt_occur.cc` — Implementation Plan (2026-05-08)

> **ARCHIVED-IN-PLACE 2026-05-27**: This doc is historical (pre-strategic-doc-set or one-off RCA). Preserved here because it is still referenced from one or more KEEP docs. See [`LODE_CLEANUP_REVIEW_2026-05-27.md`](LODE_CLEANUP_REVIEW_2026-05-27.md) for the archival rationale.

---


Status: PLAN (not implemented)
Related: `OPTIMIZER_REPORT_2026-05-07.md` §4.1 (foundational analyses),
`REVIEW_2026-05-08.md` recommendation #8.

## Why

The v3 optimizer pipeline lacks two foundational analyses (occurrence
counting; use-def map).  Almost every interesting downstream pass —
selector thunks, demand analysis, heuristic inliner, single-entry
thunk elision, smarter DCE — depends on at least one of them.  This
plan covers occurrence; the use-def map is a follow-on.

GHC's `OccurAnal` is the reference.  The Nix-shaped MVP is simpler:
no algebraic data types, no loop-breakers (no inliner yet), no
branch-aware "OneOcc.oneBranch" refinement.  Just per-`VarId`
counts and a captured/non-captured distinction.

## What

For every `VarId` in the module, classify into one of:

| OccKind        | Meaning |
|----------------|---|
| `Unknown`      | Out-of-range or not analysed.  Consumers must be conservative. |
| `Param`        | Function `paramVar`, `LetRec::recVar`, hidden-entry `hiddenVar`.  Externally bound; count is meaningful but consumers should not drop the binding. |
| `Dead`         | 0 syntactic references across the whole module. |
| `OnceLinear`   | 1 reference, in the same function as the def, not through a Lambda/MkThunk capture list. |
| `OnceCaptured` | 1 reference, but the use is inside a different function (the binding is captured by a Lambda or MkThunk body). |
| `Many`         | ≥ 2 references. |

### Authorization table (consumers MUST respect)

| OccKind        | Substitute RHS at use? | Drop binding? |
|----------------|---|---|
| Unknown        | NEVER | NEVER |
| Param          | NEVER | NEVER (externally bound) |
| Dead           | n/a   | YES (subject to existing purity check) |
| OnceLinear     | YES (even non-trivial RHS) | YES (after substitution) |
| OnceCaptured   | ONLY if RHS is trivial | NEVER automatically |
| Many           | ONLY if RHS is trivial | NO |

Trivial RHS = `LitInt/Float/Bool/Null/String/Path`, `VarRef`,
`LitPrimOp`, `LitBuiltins`.  These have zero evaluation cost so
duplicating them is free.

### `with` boundary discipline

A `OnceLinear` binding whose sole use is `With::attrs` is technically
substitutable.  But consumers must NOT substitute *into* the With's
body block — that crosses a scope boundary.  This is a consumer
policy concern (the inliner must respect it); the analysis itself
treats `With::attrs` as a normal one-use site.

### Branch-aware refinement (deferred)

GHC distinguishes `OneOcc { oneBranch: Bool }` for "use on one
exclusive branch path."  In `if c then x else 1`, `x` is used once
per execution.  The MVP conservatively counts `if c then x else x`
as 2 syntactic uses → `Many`.  Correct (no miscompile) but leaves
gain on the table.  v2 of the pass adds branch arms.

## Phase A — Standalone analysis (PR1, ~4 days)

### A.1 Extract shared helper

`computeFreeVars` (`ir.cc:336–353`) builds a `funcOfBlock[bid] → fid`
array for its reverse-deps work.  Extract into a public helper
`std::vector<FuncId> computeFuncOfBlock(const Module &)` (~30 LOC).
Both `computeFreeVars` and `analyseOccurrence` consume it.

### A.2 Public types in `include/v3/ir.hh`

```cpp
enum class OccKind : uint8_t {
    Unknown = 0,
    Param,
    Dead,
    OnceLinear,
    OnceCaptured,
    Many
};

struct OccInfo {
    OccKind  kind         = OccKind::Unknown;
    uint16_t count        = 0;     // saturating; >= 2 ⇒ Many
    bool     capturedUse  = false; // any use in a different function than def
};

struct OccMap {
    std::vector<OccInfo> data;
    OccInfo lookup(VarId v) const noexcept {
        return v < data.size() ? data[v] : OccInfo{};
    }
};

/// See lode/OPT_OCCUR_PLAN_2026-05-08.md for the full contract.
OccMap analyseOccurrence(const Module & m);
```

### A.3 `opt_occur.cc` — algorithm

```
analyseOccurrence(m):

  // 1. Scaffolding
  funcOfBlock = computeFuncOfBlock(m)
  defFunc[var] = fid for every:
                   - binding's var
                   - paramVar of every Function
                   - LetRec::recVar
                   - hiddenEntry::hiddenVar
  result.data.resize(m.nextVar)

  // 2. Initial kind for every VarId
  For each binding (bid, var, expr):
      result.data[var].kind = OccKind::Dead
  For each Function f with f.paramVar != kInvalid:
      result.data[paramVar].kind = OccKind::Param
  For each LetRec entry / hiddenEntry:
      result.data[recVar].kind = OccKind::Param
      result.data[hiddenVar].kind = OccKind::Param

  // 3. Count operand refs (NOT captured-list entries)
  For each block bid in 1 .. nextBlock:
      fid_use = funcOfBlock[bid]
      For each binding's expr:
          visitDirectOperandVars(expr, recordUse(_, fid_use))
      visitTerminal(b.terminal, recordUse(_, fid_use))

  recordUse(var, fid_use):
      info = &result.data[var]
      if info.kind == Unknown: return     // out-of-range guard
      info.count = saturating_inc(info.count)
      if defFunc[var] != fid_use:
          info.capturedUse = true

  // 4. Finalise
  For each v with kind in {Dead, OnceLinear, OnceCaptured, Many}:
      if count == 0:                          kind = Dead
      else if count == 1 && !capturedUse:     kind = OnceLinear
      else if count == 1 &&  capturedUse:     kind = OnceCaptured
      else:                                   kind = Many

  return result
```

The `visitDirectOperandVars` visitor mirrors `collectExprRefs`
**minus** the captured-list entries:
- `Lambda::freeVars` — SKIP (derived data; populated by `computeFreeVars`).
- `MkThunk::freeVars` — SKIP (same).
- `LetRec::entries[i].outerUpvalues` — SKIP (same).
- `LetRec::hiddenEntries[i].outerUpvalues` — SKIP (same).

Counting those would double-count: the upstream operand is already
referenced via the function body's direct operands.  This is the
single subtle correctness invariant of the pass.

### A.4 Pipeline integration

**No change to `optimise()` in PR1.**  The pass is exported but not
invoked automatically.  This keeps PR1 reviewable and lets us
A/B-validate before changing the hot path.

### A.5 Tests in `smoke.cc`

Minimum corpus (each builds the IR by hand and asserts OccInfo):

| Case | Expected |
|---|---|
| Empty module | `OccMap.data.size() == 1` (slot 0 = kInvalid sentinel) |
| `let x = 1 in []` | `x` Dead, count 0 |
| `let x = 1 in x + 1` | `x` OnceLinear, count 1 |
| `let x = 1 in x + x` | `x` Many, count 2 |
| `let x = 1 in (\y: x)` | `x` OnceCaptured, count 1 |
| `\x: x + x` | paramVar Param, count 2 |
| `\x: 42` | paramVar Param, count 0 |
| `let x = 1 in if c then x else 2` | `x` OnceLinear |
| `let x = 1 in if c then x else x` | `x` Many (documents conservative behavior) |
| `let r = rec { a = 1; b = a; }; in r.a + r.b` | recVar Param; `r` counts as Many |
| `with attrs; foo` | `attrs` OnceLinear (single use at `With::attrs`) |
| Nested-2 capture: `\x: \y: x` | `x` OnceCaptured at depth 2 |
| **Double-count regression** | run `computeFreeVars` then `analyseOccurrence`; assert no var's count exceeds its true syntactic ref count |

The double-count regression test is the single load-bearing
correctness assertion.  If `Lambda::freeVars` ever leaks into the
visitor, it triggers.

### A.6 Acceptance criteria

- All `make check` smoke tests pass.
- 142/142 lang tests stay green.
- Cutover-parity unchanged.
- The double-count regression test passes both pre-`computeFreeVars`
  and post-`computeFreeVars`.

**Effort:** 4 days (3 days code + tests + reviewer round-trip + 1 day
buffer for the shared-helper extraction and authorisation-table
documentation).  ~280 LOC total: ~30 helper, ~50 ir.hh, ~150
opt_occur.cc, ~50 tests.

## Phase B — DCE migration (PR2, ~2 days)

Replace `deadBindingElim`'s 8-iteration fixed-point with one pass
that consults `analyseOccurrence`'s output.  Specifically:

1. Run `analyseOccurrence` once.
2. Walk every block; for each binding where
   `occ.lookup(bind.var).kind == Dead && exprIsPure(bind.expr)`,
   remove.
3. Re-run `analyseOccurrence` (some bindings now Dead because their
   sole consumer was just removed); repeat step 2 once.
4. Done.  Two passes is enough in practice (existing DCE comment
   confirms 1–2 passes typical).

**Side-by-side validation.**  Run old DCE and new DCE on every
functional test; assert identical bindings removed for one release.
Then retire the old.

**Effort:** 2 days including the side-by-side harness.

## Phase C — Future consumers (separate sprints)

- **Heuristic inliner** — substitutes `OnceLinear` bindings with
  non-trivial RHS at the use site.  Pairs with size-cost heuristic.
- **Selector thunks** — recognises `OnceCaptured` whose RHS is an
  `AttrSelect` chain; lowers to `OP_MAKE_SELECTOR_THUNK`
  (GHC `eval_thunk_selector` analogue).
- **Single-entry thunk elision** — `OnceLinear` on a `MkThunk`
  binding whose body is referenced exactly once: skip the update
  barrier (the thunk is forced exactly once anyway).
- **Demand analysis** — uses occurrence as input; generalises
  `elimRedundantForce` across calls.

## Risk register

| Risk | Mitigation |
|---|---|
| Counting `Lambda::freeVars` entries double-counts captured operands | Visitor explicitly EXCLUDES freeVars vectors; documented; double-count regression test asserts |
| Synthetic VarIds (paramVar / recVar / hiddenVar) classified as Dead | Phase 2 of algorithm marks them `Param` before counting |
| Unreachable blocks inflate counts | Mirror existing DCE behavior (conservative; never miscompiles); document; future reachability pass can prune |
| Branch-aware refinement deferred | Documented in API contract; v2 of pass adds it; no current consumer demands it |
| Consumer misuses `OnceCaptured` for non-trivial RHS | Authorization table in API doc; code-review enforcement |
| `OccMap` staleness across mutations | Documented as freshness contract; pipeline orchestrator re-runs |
| `funcOfBlock` recomputation cost | Computed once via shared helper; sub-millisecond on typical modules |

## Out of scope

- Use-def map (separate ~300 LOC pass; `OPTIMIZER_REPORT_2026-05-07.md`
  §4.1).
- Loop breakers (no inliner yet; not load-bearing for MVP).
- Branch-aware OneOcc refinement (deferred to v2).
- `with`-shadowing handling beyond the current per-VarId scope
  (consumer policy concern; analysis is correct as-is).

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
