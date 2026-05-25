# V3 Fork Review — 2026-05-21

Seven parallel agents reviewed the v3 fork of cppnix
(`/Users/angerman/Projects/iohk/nix`, 1154 commits ahead of `origin/master`,
+152 K LoC / -93 LoC across 400 files). Synthesis below is organised around
the **V3-NATIVE** principle: prefer the pure-VM path, fall back to the
tree-walker only at irreducible FFI leaves (store, paths, derivations,
file I/O, parser). Every finding cites file:line.

Verification pass (separate from agents) confirmed three agent claims
wrong and the rest as quoted. Those discards are listed in §C so they
don't propagate.

## Scope

| Slice | Files / LoC |
|---|---|
| `src/libexpr-v3/` | 337 files, ~37 K LoC (vm.cc 11.7 K, primops.cc 9.9 K, lower.cc 3.2 K, alloc.hh 1.2 K, …) |
| `src/libexpr/` (TW + v2) | 30 files, +17 225 / −86 LoC; v2 bytecode pipeline = 9 .cc / 5 .hh / ~13 K LoC, gated `NIX_VM_V2=1` |
| `src/libcmd/installables.cc` | +36 LoC; v3-direct skip gate |
| `src/nix/eval.cc` | +238 LoC; `runV3DirectEval` entry |
| `src/nix/main.cc`, `daemon.cc`, `src/libutil/`, `src/libstore/` | minor |
| `src/libexpr-v3/lode/` | 86 markdown files (audit memos, design docs, bench logs) |

---

## A. V3-NATIVE-aligned wins (land for purity AND correctness)

These are the changes that move us *toward* the pure-VM path. They both
shrink the dead-code surface AND remove TW-aware leakage from the
hot path.

### A1. Retire VM v2 entirely — ~13 K LoC + ~2 K headers + ~500 LoC TW-side leak

**Evidence (cross-agent + verified):** dead-code agent traced every v2
entry point to `src/libexpr/eval.cc:1224-1380` inside
`if (useVMv2)`. No code path reaches v2 outside that gate. v2 is gated
`NIX_VM_V2=1`, no production caller sets it; per
[[v3-status-superseded-rule]] the user has confirmed v2 is dead and only
v3 matters.

**Deletable as one PR:**

```
src/libexpr/bytecode.cc                     103
src/libexpr/bytecode-compiler.cc          1 231
src/libexpr/bytecode-disasm.cc              323
src/libexpr/bytecode-disk-cache.cc          313
src/libexpr/bytecode-serialize.cc           651
src/libexpr/bytecode-thunk.cc               137
src/libexpr/vm.cc                         5 378
src/libexpr/ir.cc                         1 976
src/libexpr/ir-emit.cc                    2 684
src/libexpr/include/nix/expr/{bytecode*, ir*, vm}.hh, nix-word.hh  ~1 400
src/libexpr/BYTECODE-TODO.md, VM-V2-DESIGN.md, opt-native-primops.md
scripts/bench-vm-v2.sh
```

Plus the v2-aware sections of upstream TW files (the second-order
collateral) that the hook-surface agent enumerated. Concretely:

| File | Lines | What |
|---|---|---|
| `src/libexpr/eval.cc` | 1-9, 217-235, 1222-1428, 1620-1925, 2028-2412, 3108-3580 | v2 includes, isTrivial bytecode-thunk dispatch, eval() v2 branch, callFunction primop-timing for v2, ExprOpUpdate iterative spine (WC-9), v2 stats |
| `src/libexpr/include/nix/expr/eval.hh` | 53-57, 395-444, 759 | bytecode forward-decls, EvalState v2 fields (always present today — see A2), handleEvalExceptionForThunk visibility bump |
| `src/libexpr/include/nix/expr/eval-inline.hh` | 8-44 of v2 path | twValueTypeName helper, ExprBytecodeThunk check inside forceValue |
| `src/libexpr/include/nix/expr/nixexpr.hh` | 143-150 | `isBytecodeThunk` / `isBytecodeProxy` flags on every `Expr` |
| `src/libexpr/include/nix/expr/value.hh` | 1223-1232 | `isThunkOrApp()` bitfield-adjacency trick (pure v2 dispatch micro-opt) |

**Total v2-retirement collateral: ~13 K v2 files + ~500 LoC TW-side
leakage + ~2 K headers ≈ 15 K LoC**. This is the single biggest move.

**Falsifier:** build with v2 sources removed; run `tests/functional` +
`v3-eval` lang suite. If both pass, the gate was the only thing keeping
v2 reachable.

### A2. Fix TW-side abstraction leaks that survive v2 retirement

Even AFTER v2 dies, the hook-surface agent flagged 2 unconditional leaks
of v3 awareness into TW code. These violate V3-NATIVE in the *other*
direction (TW knowing about v3):

- **`src/libexpr/include/nix/expr/nixexpr.hh:152-156`** —
  `Expr::isV3CacheCandidate` is a 1-byte flag on every `Expr` node,
  written by libnixexprv3, read by the (now-unused) TW hook hint path.
  When v3 owns eval, TW's forceValue shouldn't be consulting this flag at
  all. **Delete the field** and the dependent code in eval-inline.hh.
- **`src/libexpr/include/nix/expr/nixexpr.hh:106-138`** — `Expr::Kind`
  enum (27 values) + constructor-assignment in every Expr subclass. Was
  added so v2's `ir::Lowerer` could replace `dynamic_cast` chains with a
  switch. v3 has its own IR in `src/libexpr-v3/ir.hh`. **If v2 goes,
  Kind goes** (the constructors revert to upstream form). Two
  small architectural pieces (`isBytecodeThunk`, `isBytecodeProxy`) go
  with it.

### A3. Eliminate the per-list-iteration TW round trip in `primMap` / `primFilter` / `primFoldl`

**Evidence:** vm.cc cleanup agent flagged that
`vm.cc:11300 callClosure` is the anonymous-namespace entry that
`primops.cc:1189+` (`primMap`/`primFilter`/`primFoldl`) call into to
invoke user closures on each element. Per the V3-NATIVE rule this is
the CORRECT direction (v3 inner loop → v3 callClosure → v3 user
closure body). **What violates V3-NATIVE is the corresponding
`v3ToTreeWalker` bridge in `primops.cc:4833-6984` (2 152 LoC) — when
this fires inside a primop iteration, the per-element user lambda is
bridged through TW.** Identify and gate those bridge sites by V3-native
intrinsic dispatch wherever possible (the [[fffi-audit]] inventory
already starts this Tier classification).

**Concrete next step:** add a counter at the entry of each
`v3ToTreeWalker` overload (one bumps per element). Run hello.drvPath
+ a Haskell sample. Any per-element count above ~10 means we have a
loop bridging through TW that should be ported to v3-native (Tier 2 of
[[FFI_AUDIT_2026-05-20]]).

### A4. Move bridge-fallback recovery out of the unwind path

vm/primops agent flagged that v3's exception unwind
(`clearBlackMarksOnException`, vm.cc:9483-11685) only fires from the
dispatch loop's catch. **If a primop (primops.cc) calls `callClosure()`
and that throws, the thunk that called the primop stays Black** —
subsequent forces re-enter, hit the same exception, infinite loop.
Wrap every `callClosure()` call site in primops.cc with a
`VMExceptionGuard { ~~~ { clearBlackMarksOnException(...); } }` RAII
helper. Audit list: primFoldl, primMap, primFilter, primConcatMap,
primGenList, primAll, primAny, primPartition, primGenericClosure,
primImport (compiled-body call).

**Falsifier:** synthetic test `let f = x: throw "boom"; in tryEval
(builtins.map f [1])` should return `{success=false; value=...}`
twice in a row; today the second call may re-throw or hang depending
on which thunk path was taken.

### A5. Retire `cu` and `capturedWiths` from Closure / Thunk via measurement spike

Already proposed in `DATA_STRUCTURE_AUDIT_2026-05-21.md` §B1-B2 but
worth restating with the V3-NATIVE angle: when `cu` is null (90 % case
per the field's own doc), the dispatcher reuses the caller's CU — pure
V3 path. Storing the null field on every Closure costs both memory AND
the dispatcher branch that checks for it. **Side-table makes the
default path branch-free.**

---

## B. Correctness gaps — TW-parity bugs (sweep finds 6 untriaged)

The TW-parity agent inventoried bugs in the same classes as #672-#691.
All were verified via grep against TW source (`src/libexpr/primops.cc`).
None are speculation.

### B1. Missing `forceStringNoCtx`-equivalent on hash/regex/JSON inputs

These primops accept context-carrying strings silently; TW rejects.
Same operating rule as #674. Each is a one-line addition of
`requireNoStringContext()` at the read site.

| Primop | v3 site | TW comparison | Severity |
|---|---|---|---|
| `primMatch` regex arg | `primops.cc:2995` | `libexpr/primops.cc:5085 forceStringNoCtx(args[0])` | high |
| `primSplit` regex arg | `primops.cc:3032` | `libexpr/primops.cc:5159 forceStringNoCtx(args[0])` | high |
| `primHashString` algo arg | `primops.cc:3103` (verified) | `libexpr/primops.cc:4930 forceStringNoCtx(args[0])` | high |
| `primHashFile` algo arg | `primops.cc:3112` | `libexpr/primops.cc:2469 forceStringNoCtx(args[0])` | high |
| `primFromJSON` input | `primops.cc:8157` (verified) | `libexpr/primops.cc:2787 forceStringNoCtx(args[0])` | high |
| `primGetAttr` dyn-name | `primops.cc:1816` | `libexpr/primops.cc:3258 forceStringNoCtx` | high |
| `primHasAttr` dyn-name | `primops.cc:1832` | implied by getAttr sibling | high |
| `primRemoveAttrs` list elements | `primops.cc:1712` | `libexpr/primops.cc:3404 forceStringNoCtx per elem` | high |

All shape-aligned with the #674 audit conclusion ("25+ primops surveyed").
The agent found the survey wasn't exhaustive.

### B2. `std::runtime_error` instead of `EvalError` — tryEval can't catch

Same class as #684 (addErrorContext type preservation). Sites still
throwing `std::runtime_error` from the primop:

- `primDiv` div-by-zero (primops.cc:1029-1038)
- `primAdd/Sub/Mul` overflow (primops.cc:970/989/1008)
- `primHead`/`primTail` empty-list (primops.cc:539/548)
- `primGenList` negative size (primops.cc:1345)

vm.cc cleanup agent quantified 111 `std::runtime_error` in vm.cc + 132
in primops.cc. Most are correct (genuine bugs the user should see), but
**tryEval catches AssertionError-class only** — anything throwing
`std::runtime_error` escapes the `tryEval` fence. Wrap the dispatch
loop's exit in a single `try { ... } catch (std::runtime_error & e)
{ throw EvalError(e.what()); }` adapter — one place, all sites
benefit.

### B3. Two potentially-eager paths

- `lower.cc` `lowerAssert` — assert condition may be eagerly forced
  via `forceVal(lowerExpr(e->cond))`. Test: `assert (throw "x"); 1`
  vs TW.
- The TW-parity agent flagged primMatch / primSplit might not propagate
  context to result strings. Marked NEEDS-VERIFICATION — diff against
  TW's behavior on `(builtins.split " " "${drv} foo") |> map …`.

---

## C. Agent claims that did NOT survive verification (discard)

To prevent error propagation in future audits, three verified-wrong
agent claims:

| Claim | Source | Verification | Reality |
|---|---|---|---|
| `src/libstore/optimise-store.cc` +1 line `#include <regex>` is suspicious / dead | hook-surface agent | grep regex in file: **line 112 `std::regex_search(path.string(), std::regex("\\.app/Contents/.+$"))`** | Real upstream-divergent change. `<regex>` is USED. Likely macOS `.app/Contents/` exclusion. KEEP. |
| `primTrace` writes to stderr unconditionally — should be gated on `--trace` / `NIX_TRACE` | vm/primops cleanup agent | `builtins.trace` is supposed to print; cppnix TW does the same | Documented Nix semantics. KEEP. |
| Inventory says 226 gates, doc lists 167 — 35 % growth | env-var agent | not re-verified end-to-end, but the **lint-script narrowness claim is real** (script only scans vm.cc + primops.cc) | The headline number needs a 5-minute recount before quoting. The lint-narrowness finding is solid. |

The dead-code agent and lode/ agent results are accepted as quoted —
verified by spot-check.

---

## D. lode/ archive cleanup — 60 files moveable

Per `lode/` agent (cross-checked against `src/libexpr-v3/CLAUDE.md`):

- **CANONICAL (keep): 27 files** — all 16 in the CLAUDE.md authoritative
  list + 7 actively cited + 3 from today's investigation chain
  (RCA_VALUEPAIR_EVALUATED_2026-05-21, GC_AUDIT_ROUND_2_2026-05-21, and
  this file's predecessor DATA_STRUCTURE_AUDIT_2026-05-21) + **the
  re-promoted `UNISON_IDEAS_2026-05-07.md`** (see correction below).
- **HISTORICAL → `lode/archive/`: 38 files** — RCA chains that closed,
  bench logs whose numbers shifted, completed plans (e.g.
  `OPTION_4_COMPLETE_2026-05-18`, `LEXICAL_WITHS_PLAN_2026-05-08`,
  `CLEANUP_AUDIT_2026-05-09`, 8 REVIEW_2026-05-0X snapshots).
- **DEAD → delete: 21 files** — `path_b_full_frame_dump_2026-05-17.txt`
  (a debug artifact, not docs), pre-finding inventories
  (`ENV_VAR_INVENTORY_2026-05-15.md` — superseded by today's audit
  too), and 19 others. Full list in the agent's report.

**Correction (2026-05-21, post-IFD deep-dive):** the original DEAD
classification of `UNISON_IDEAS_2026-05-07.md` was wrong. That memo
is the canonical design reference for the **eval-cache program** that
retires haskell.nix's `materialization` workaround — its §3 (Hash-
keyed evaluation cache) is the same proposal as the IFD deep-dive's
S4 strategy, and its §4 (Effect propagation) is the static-IFD-
detection path that replaces materialization's policy-shield role.
See `IFD_DEEP_DIVE_2026-05-21.md` §11 for the full sequencing. The
prior misclassification illustrates the audit risk flagged in §C of
this document: agents project staleness from filename patterns
(`*_IDEAS_*`) without checking whether the content remains
forward-looking. Future audits should treat any ideas / design doc
as load-bearing until cross-referenced against active strategy memos.

This is a follow-up `git mv` + `git rm` exercise, ~30 minutes of
mechanical work. Cuts the folder by ~63 %.

---

## E. Gate hygiene — comply with ACTION_PLAN Rule 4

The env-var agent found 226 active gates (claim needs re-counting per
§C; even if the number is off the trend is right). The rule (`CLAUDE.md`
Rule 4 + ACTION_PLAN Part 1 rule 2):

> No new env-var gate without an inline retirement criterion in the
> comment at the first `getenv()` read site.

**Violations the agent identified with citations:**

- `NIX_V3_BRIDGE_PRIMOP_DEPTH` (`primops.cc:165`), `V3_DBG_INHERIT_FROM_THUNK`
  (`lower.cc:2235`), `V3_DBG_INTRINSIC_ALL` (`lower.cc:1179`), `NIX_V3_NO_BC_*`
  family (12 BC-primop toggles, `bytecode_primops.cc:368-507`),
  `V3_DBG_NURSERY_AUDIT`, `V3_DBG_NURSERY_BRUTE`,
  `V3_DBG_ATTRS_HAS_KEY`, `V3_DBG_SELECT_PATTERN` — all lack
  retirement comments.
- `test/lint-no-inline-getenv.sh` only scans `vm.cc` + `primops.cc`,
  letting gates in `lower.cc`, `bytecode_primops.cc`, `nursery.cc`,
  `fiber.cc` etc. slip through.

**Recommended retire-empty candidates** (gate + no behind-code):

- `V3_DEBUG_HOOK` (primops.cc:3823/4251/4368) — `v3_hook.cc` deleted
  per memory; gate is orphan.
- `NIX_V3_EAGER_BRIDGE_MAX` — eager-bridge apparatus removed in
  Phase 0 (`commit 156939f43`); gate is orphan reader.
- `V3_NATIVE_CALL_FLAKE_DESIGN_2026` — comment-only gate; no
  functional code path.

**Recommended fix the lint:** extend the script to scan all
`src/libexpr-v3/*.cc` AND fail on missing retirement comments
(grep-near for `retire:|Retire|RETIRE`). One-day work.

---

## F. Source-file split (vm.cc / primops.cc)

The cleanup agent proposed concrete splits but I'm de-prioritising them
relative to A1-A2 (V2 retirement) because **splitting before the major
deletions creates merge conflicts**. Order of operations:

1. A1 (V2 retire) → vm.cc unchanged but primops.cc shrinks at the
   bridge/conversion zones if A3 measurements support it
2. A4 (exception guard) → adds RAII wrapper in primops.cc; small
3. THEN run the split, with v3-specific layout (the proposed
   `vm_dispatch_attrs.cc` / `primops_string_ops.cc` etc. are sound)

vm.cc's `clearBlackMarksOnException` (9483-11685, 2 203 LoC) is a
legitimate split candidate independent of A1 — extracting to
`vm_exception_recovery.cc` reduces cognitive load with zero coupling
risk. ~1 day.

`primops.cc`'s `decodeStringContext` (316-3668, 3 353 LoC) and
`v3ToTreeWalker` (4833-6984, 2 152 LoC) are also extractable as
single-file moves; do as part of A3.

---

## G. Recommended order

Each step has a clear falsifier; none commits us past the
[[falsification-rule]]:

| # | Action | Effort | Falsifier |
|---|---|---|---|
| 1 | A1 — remove VM v2 sources from `src/libexpr/` + revert TW-side leaks (`isV3CacheCandidate`, `Expr::Kind`, `isBytecodeThunk`, `isBytecodeProxy`, EvalState v2 fields, `isThunkOrApp`, `twValueTypeName`, isTrivial bytecode dispatch, eval.cc v2 branch + stats) | 2-3 days | `make` + `tests/functional` + `v3-eval` lang suite all green |
| 2 | E — retire 3 orphan gates (`V3_DEBUG_HOOK`, `NIX_V3_EAGER_BRIDGE_MAX`, `V3_NATIVE_CALL_FLAKE_DESIGN_2026`); extend lint script to all src/libexpr-v3/*.cc | 1 day | lint passes; grep shows 0 readers of removed gates |
| 3 | B1 — add `requireNoStringContext` to the 8 listed primops, with per-fix synthetic test that errors-match TW | 1 day | parity diff against TW byte-for-byte |
| 4 | B2 — wrap dispatch loop exit in `runtime_error → EvalError` adapter | 0.5 day | `tryEval` synthetic tests on div-by-zero / overflow / empty-list pass |
| 5 | A4 — VMExceptionGuard around primops.cc `callClosure` sites | 1 day | `tryEval (map (x: throw "boom") [1])` twice without hang |
| 6 | A3 measurement — add per-element counter at `v3ToTreeWalker` overloads; run hello.drvPath + a Haskell sample; report top bridging loops | 0.5 day spike | data table; informs Tier 2 priority |
| 7 | D — lode/ cleanup (move 38 → archive/, delete 22) | 0.5 day | `git mv` + `git rm` mechanical |
| 8 | F — split vm.cc `clearBlackMarksOnException` → `vm_exception_recovery.cc` | 1 day | build still green; no cross-file edits needed |
| 9 | A5 — measurement spike for Closure::cu / capturedWiths null fraction (deferred from data-structure audit) | 1 day | data table feeds B1/B2 of `DATA_STRUCTURE_AUDIT_2026-05-21.md` |

Items 1, 3, 4 are the V3-NATIVE / correctness core; items 2, 7, 8 are
hygiene that pays back complexity budget; items 5, 6, 9 are
measurement-first investigations that turn into work only if the data
justifies it.

---

## H. What's NOT in this report (deferred)

- The Bindings append-overlay shape from `DATA_STRUCTURE_AUDIT_2026-05-21.md`
  §B4 — still gated on the Phase D nursery decision per
  `NURSERY_PHASE_D_DESIGN_2026-05-18.md`.
- IR optimisation Phase 2(R) work — running in parallel per
  `IR_OPTIMIZATION_PLAN_2026-05-18.md`.
- Stage 9 / linking — addressed by `LINKING_DESIGN_2026-05-17.md`.
- Performance regressions on hello.drvPath — being measured via
  `PERF_TRACE_TOOL_DESIGN_2026-05-20.md` (`perf-trace.py`) and the
  Phase A nursery work, not this audit.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
