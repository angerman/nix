# lode/ critical review — 2026-05-18

Critical review of `src/libexpr-v3/lode/` for gaps, contradictions, and missing infrastructure. Also answers three specific questions: (1) is IR serializable? (2) is the optimizer pipeline clear? (3) would FileCheck-style tooling help?

## 1. lode/ folder state — 78 docs, growing

Total documents in `src/libexpr-v3/lode/` (including the `REVIEW_2026-05-11/` subdir): **78 markdown files, ≈25 000 lines total**. Categorized:

| Category | Count | State |
|---|---|---|
| Canonical strategic set (auto-loaded via CLAUDE.md) | 10 | Good — all 2026-05-15-or-later, cross-referenced, current |
| Closed RCA series (RCA_FAMILY_DIVERGENCE_*) | 5 | **No RESOLVED markers** despite the A5/A7 bug closing |
| Chronological reviews (REVIEW_2026-05-0X) | 7 | Mostly superseded; consider consolidating |
| REVIEW_2026-05-11/ multi-agent reports | 10 | Largely superseded by the new canonical set |
| Old plans (FFI_PLAN, WC38_FIX_PLAN, INVERSION_PLAN, LEXICAL_WITHS_PLAN, etc.) | ~10 | Need RESOLVED/DEFERRED triage |
| Old bench docs (BENCH-2026-05-0X) | 5 | Stale; superseded by current baseline |
| Audits | ~8 | Mostly point-in-time |
| Misc (UNISON_IDEAS, WITH, GC-REVIEW, etc.) | ~13 | Some still relevant |

**RESOLVED-marker scan**: 10 docs out of 78 have any RESOLVED/CLOSED/FIXED indicator. Most of those are recent strategic docs that self-mark phases as MET. The **historical investigation/RCA docs almost never have closure markers**, even when the bug they investigate is closed (e.g., the 5 RCA_FAMILY_DIVERGENCE docs investigated a class of bugs closed by commits `1708d31bd` / `1ac5795b0` / `7adc7e61f` — none of those resolution commits appear as RESOLVED markers on the RCA docs).

This violates the action plan's standing weekly cadence rule: *"Every closed bug: append a one-line RESOLVED row to its lode/ doc."*

### 1.1 Specific lode/ gaps

| # | Gap | Severity | Recommended action |
|---|---|---|---|
| L1 | 5 RCA_FAMILY_DIVERGENCE_*.md docs lack RESOLVED rows | Medium | Append "RESOLVED 2026-05-15 via Option 4 hybrid (commit `7adc7e61f`)" to each |
| L2 | OPTIMIZATION_PLAN.md is 3 774 lines, chronological log, "Phases 1-4 landed" | High | Rename `OPTIMIZATION_PLAN_HISTORICAL.md` and add at the top "SUPERSEDED by IR_OPTIMIZATION_PLAN_2026-05-18 + ROADMAP Stage 4. Read this only for historical context." |
| L3 | 2 FFI_PLAN docs same-day (06 + 06b) | Low | Consolidate into single FFI_PLAN doc; cite the 06b corrections inline |
| L4 | 7 REVIEW_2026-05-0X chronologicals | Low | Move to `lode/archive/early-may/`; add a one-line README pointing forward |
| L5 | REVIEW_2026-05-11/HONEST_ASSESSMENT and PROGRESS_* (~7 docs) | Medium | Many were inputs to the 2026-05-15 strategic doc set; mark superseded |
| L6 | 5 stale BENCH-2026-05-0X docs | Low | Move to `lode/archive/bench/`; baseline JSON is canonical |
| L7 | WC38_FIX_PLAN.md status: "design notes, not implementation" but the bug WAS fixed | Medium | Add RESOLVED row citing `c95be6461 + 685262a6f` (per memory) |
| L8 | INVERSION_PLAN_2026-05-08 | Medium | Phases 1-4 partly landed; doc isn't updated. Add "Status: superseded by ROADMAP Stages 2-9" |
| L9 | LEXICAL_WITHS_PLAN_2026-05-08 has a RESOLVED row but it's not immediately clear | Low | Verify and clarify |
| L10 | No `lode/archive/` subdirectory | Medium | Create it for historical docs; canonical set stays at top level |

**Net recommendation**: clean up to ~15-20 active docs at top level + everything else in `lode/archive/`. Cuts cognitive load substantially. Per the action plan's "no new lode/ doc until previous closes" rule, this is overdue.

### 1.2 lode/README.md currently references the canonical 4-doc set

The README correctly flags the 4-doc canonical set at top, but:
- The "Reviews & audits" / "Plans (forward-looking)" sections still list old docs without RESOLVED markers
- The "Drop new artifacts here as they age" instruction works only if old artifacts get retired

Recommendation: add a `## Closed investigations (with RESOLVED commit pointers)` section. Each closed RCA gets one line: `RCA_FAMILY_DIVERGENCE_2026-05-11.md — RESOLVED 2026-05-15 via Option 4 hybrid (commit 7adc7e61f). Read only for historical context.`

## 2. IR serializability

**Answer: NO, IR is in-memory C++ structs only.**

What we have:
- `ir.cc` (483 LoC) — in-memory IR data structures (Module, Function, Block, Binding, VarId, ...).
- `ir_dump.cc` (518 LoC) — **text dump only**, debug-oriented, no parser counterpart. One-way: IR → text.
- `serialize.cc` (596 LoC) — serializes **bytecode** (the post-IR-lowering output: opcodes + const pool + lambda descriptors), not IR. Used by `disk_cache.cc` for the per-file bytecode SQLite cache.

What this means:
- Cannot snapshot IR between optimizer passes for cross-pass debugging.
- Cannot persist IR for content-addressed sharing (Stage 9 LINKING_DESIGN Phase L0 needs `structuralHash()` over IR — possible without full serialization, but neighbouring work would benefit).
- Cannot use IR as a stable input format for `.nix-or-IR` fixture testing (the FileCheck story; see §4).
- Cannot use IR for offline analysis tooling (e.g., visualize control flow, audit shape statistics).

### 2.1 What's needed

Three escalating levels of IR serializability:

(A) **Text dump with canonical form** (extension of ir_dump.cc). Output is deterministic given the same IR; same IR always produces byte-identical output. Useful for FileCheck-style assertions (next section). No parser. ~1-3 days of work to tighten ir_dump.cc.

(B) **Text round-trip** (add an IR parser to ir_dump.cc). Lets us write `.ir` fixture files, load → optimize → dump → assert. Enables stable on-disk IR test fixtures. ~1-2 weeks.

(C) **Binary serialization** for performance (used by content-addressed cell hashing in LINKING_DESIGN Phase L1 ABT refactor + Stage 9 cell store). Stable wire format with explicit versioning, ABI-hash salt. ~2 weeks; closely coupled to LINKING_DESIGN L0/L1.

**Recommendation**: ship (A) immediately (low cost, high leverage for testing). (B) is a Stage 9 prerequisite. (C) is Stage 9 substrate.

### 2.2 What's already a useful starting point

`ir_dump.cc` already produces line-oriented text — the same format the existing `checkIr` function (§4) consumes. The output appears canonical-ish (we'd need to audit determinism for things like map iteration order). The first cost is auditing for canonicality, then exposing it as a CLI subcommand.

## 3. Pipeline between phases

**Answer: yes-but.** The pipeline IS documented in code and a plan doc, but no canonical reference exists, per-pass contracts aren't formalized, and fix-point iteration is planned but not implemented.

### 3.1 Current state

The optimizer pipeline lives in `opt_const_fold.cc::optimise(Module&)`. Today's order:

```
1. constantFold              (literal arithmetic)
2. betaReduce                (NEW 2026-05-18: 1-shot lambda inlining; Phase A)
3. constantFold              (re-run; betaReduce exposes new constants)
4. commonSubexprElim         (block-local CSE)
5. elimRedundantForce        (#423: Force-chain collapse)
6. inlineTrivialBindings     (VarRef path compression)
7. fusePrimOpApps            (#429: App-chain → PrimOpCall)
8. deadBindingElim           (or deadBindingElimViaOccur gated)
```

Inline comments explain WHY each pass runs WHERE (this is good). The header docstring of each `opt_*.cc` describes the pass.

Per `IR_OPTIMIZATION_PLAN_2026-05-18.md` §2.4, after IR Phases B-H land:

```
1. constantFold              (extend: known-pure primops)         [B]
2. betaReduce                (already landed)                     [A]
3. constantFold              (re-run)
4. commonSubexprElim         (unchanged)
5. elimRedundantForce        (unchanged)
6. inlineTrivialBindings     (unchanged)
7. streamFusion              (foldl'/map/filter)                   [C]
8. genListUnroll             (small static genList)                [H]
9. ifThenFold                (static-condition If)                 [G]
10. lambdaLift               (capture-free Lambda)                 [D]
11. selectorRecognize        (mark x: x.sym lambdas)               [E]
12. fusePrimOpApps           (unchanged)
13. staticAppSpineFold       (fully static App spines)             [F]
14. deadBindingElim          (unchanged)
```

Plus fix-point iteration on steps 1-3 + 7 + 13, capped at max-iterations.

### 3.2 Gaps

| Gap | Impact |
|---|---|
| No canonical `OPTIMIZER_PIPELINE.md` doc | Reader has to read `opt_const_fold.cc` source + IR_OPTIMIZATION_PLAN to understand the full pipeline. Action plan Phase 4 has this as a TODO; not yet done. |
| Per-pass input/output invariants not formalized | E.g., "betaReduce assumes every Lambda has correct freeVars; produces Modules where every inlined VarRef points to a Binding in scope." Not documented. Bugs that violate invariants are hard to attribute. |
| Fix-point iteration not implemented | Per IR plan §2.4. Today the pipeline runs once. Multi-pass interactions (e.g., a constant exposed by Phase C surfaces a new beta-reducible call) may not converge. |
| Pass enablement gates inconsistent | `NIX_V3_NO_OPT` disables all passes; `NIX_V3_OCCUR_DCE` enables one alternate path. No per-pass enable/disable matrix. |
| No regression tests for pass ORDERING | If someone reorders passes, no test catches "now beta-reduce sees pre-CSE shapes it didn't expect." |
| Pass coverage / metrics | We don't know per-pass: how many bindings did it modify? how many transformations fired? Useful for tuning. |

### 3.3 What's needed

(A) **OPTIMIZER_PIPELINE.md** — a single source-of-truth doc:
- The pass order, with rationale per ordering decision.
- Per-pass: name, input invariants, output invariants, when to enable, performance impact.
- The fix-point design (which passes are in the loop, max iterations).
- How to add a new pass.

(B) **Per-pass invariant checks** (debug-only via `V3_DBG_IR_INVARIANTS=1`):
- Pre/post-pass invariant assertions.
- Catches "pass X violated invariant assumed by pass Y."
- Cost: zero in release; significant in debug.

(C) **Per-pass enable/disable matrix** (gated via `NIX_V3_OPT_PASS=foo,bar,!baz`):
- Allows surgical bisection ("which pass introduced the regression?").
- Each pass also runs under `NIX_V3_NO_OPT_<PASSNAME>=1` for individual disable.
- Existing `NIX_V3_NO_OPT_STRICT` is the prototype.

(D) **Per-pass counters / stats**:
- Each pass exports "bindings modified," "rewrites fired."
- Exposed via `NIX_V3_OPT_STATS=1`.
- Reveals which passes are doing real work on real inputs vs no-ops.

(E) **Fix-point iteration**:
- Per IR plan §2.4, the loop iterates while any pass modifies the IR.
- Max iterations safety guard.
- Counter to detect divergence (pass A undoes what pass B does).

## 4. FileCheck-style infrastructure

**Answer: it ALREADY EXISTS in v3, just under-used.**

`ir_dump.cc` lines 416-510 implement an LLVM-FileCheck-compatible directive system:

- **`parseChecks(expected)`**: parses `; CHECK: pat` and `; CHECK-NOT: pat` directives from text.
- **`checkIr(actual, expected)`**: runs the directives against actual IR dump output, returns empty string on success or formatted diagnostic on failure (with context lines and pattern, like LLVM's FileCheck does).

Used today in `test/smoke.cc` (25 CHECK directives across 58 IR-testing functions). Example from smoke.cc:

```cpp
const char * expected = R"(
    ; CHECK: ; module n_funcs=1
    ; CHECK: ; func f0 entry=B1
    ; CHECK: B1:
    ; CHECK:   v1 = LitInt 42
    ; CHECK:   v2 = LitInt 1
    ; CHECK:   v3 = Add v1 v2
    ; CHECK:   return v3
)";
auto err = ir::checkIr(dump, expected);
```

This is structurally identical to LLVM's `opt -S < input.ll | FileCheck input.ll`. The semantics are LLVM-compatible: positional matching, CHECK-NOT between positives, etc.

### 4.1 What's missing for full FileCheck-style use

The infrastructure for IR-vs-CHECK assertion is there. What's missing is the **end-to-end test flow**:

1. **A driver binary** that takes a `.nix` file and dumps the post-lowering, post-optimization IR. We have `v3-eval` (CLI for execution) but not `v3-opt` (CLI for emit IR). The closest is `v3-smoke` running hand-built IR.

2. **Per-pass entry points** — run only Phase A; only Phase A+B; etc. Would let fixtures test individual passes.

3. **`.nix` fixtures with embedded CHECK directives**. Convention: `.nix` files where comments include `# CHECK: ` directives that `checkIr` would consume.

4. **Test runner**: walk test directory, for each `.nix` fixture, run `v3-opt --pass=A,B,...` → pipe through `checkIr`. Wire into `ninja test`.

### 4.2 Why this matters

Per the IR_OPTIMIZATION_PLAN §2 phases A-H, each phase rewrites IR. The exit criteria are stated as "bench shows X faster" or "fewer OP_CALL_PRIMOP" — but these are RUNTIME assertions, not IR-shape assertions. The per-pass behaviour is harder to validate without observing the IR directly.

Concrete examples of what FileCheck would catch that runtime tests don't:

- Phase A regression: a future change accidentally suppresses beta-reduction in a shape it should fire on. Runtime test sees similar perf (within noise); IR-shape test catches "no inlining happened."
- Phase B (primop constant fold) misses a case: runtime test eval result is identical; IR-shape test catches "the LIT_INT 5 wasn't produced."
- Phase C stream fusion misses a known shape: runtime sees only marginal slowdown; IR test catches "the fused form wasn't emitted."
- Pass ordering change introduces a regression: runtime tests pass; IR-shape tests show "Phase D now sees different inputs and produces different output."

This is exactly what LLVM uses FileCheck for. The cost of building it out is small given the library already exists.

### 4.3 Recommendation

**Do this. Specifically**:

(1) Add a `v3-opt` driver (or `v3-eval --emit-ir`): takes `.nix`, lowers, runs requested passes, dumps IR. ~1-2 days.

(2) Add per-pass entry points: `v3-opt --pass=constantFold` / `--pass=A,B,C` / `--pass=all-pre-fusion`. ~1 day.

(3) Convention for `.nix` fixtures with CHECK directives. **Updated 2026-05-18**: matches LLVM's `.ll`-file pattern — CHECK directives live in the `.nix` source file as `#` line-comments. Nix parser ignores `#`-comment lines; runner extracts `# CHECK:`, `# CHECK-NOT:`, `# RUN-OPT-PASSES:` from the same file. One file = source + directives + RUN spec. No sidecar; cleaner. Extension to `parseChecks` (~1-line change in `ir_dump.cc`): recognize both `;` and `#` as leading comment markers. See `IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md` §2 D2.5-D2.7. ~0.25 day.

(4) Test driver script `test/run-ir-checks.sh` that walks `test/ir-fixtures/`, runs `v3-opt`, pipes through `checkIr`, reports pass/fail. ~1 day.

(5) Seed corpus of ~30 fixtures covering each pass: 5 per phase A-H (positive + negative shapes), 5 for the pre-existing passes (constantFold, CSE, DCE, inline, primop-fuse). ~1 week.

**Total cost**: ~2 weeks. **Leverage**: persistent regression-catching infrastructure for every future optimizer change. This is high-yield, low-risk work that the team should prioritize alongside Phase B and Phase C (which would otherwise lack per-pass assertions).

## 5. Summary table — answers to the four questions

| Question | Answer | Recommended action |
|---|---|---|
| Is lode/ well-curated? | NO — 78 docs, ~10 RESOLVED markers, accumulation pattern visible | Triage: ~15-20 active docs at top level; move rest to `lode/archive/`. Append RESOLVED rows to closed RCAs. |
| Are IRs serializable? | NO — text dump only, one-way | Land canonical text-dump (Level A) immediately for FileCheck use. Round-trip parser (Level B) is a Stage 9 prerequisite. |
| Is the pipeline clear? | YES-but — code-level documented, no canonical pipeline doc, no per-pass contracts | Write OPTIMIZER_PIPELINE.md (action-plan Phase 4 TODO already). Add per-pass invariant checks + per-pass enable matrix. |
| Would FileCheck make sense? | YES, and **the library already exists** in ir_dump.cc | Add the driver binary + per-pass entry points + .nix fixture convention + test-runner. ~2 weeks total. |

## 6. Prioritization

For the team given the IR Phases A-H work-in-progress:

**High-priority, this-week-ish (1-2 weeks total)**:
1. Add `v3-opt` driver + per-pass entry points (3 days) — unblocks FileCheck-style testing for each new IR phase as it lands.
2. Add the `test/run-ir-checks.sh` runner + initial 10 fixtures for the already-landed passes (3 days).
3. Write OPTIMIZER_PIPELINE.md (1 day) — pulls scattered information into one place.

**Medium-priority, near-term (1-2 weeks)**:
4. lode/ archive cleanup: move historical docs; append RESOLVED rows; consolidate FFI_PLAN; rename OPTIMIZATION_PLAN_HISTORICAL (2 days).
5. Per-pass invariant checks (debug-mode) (3 days).
6. Per-pass enable/disable matrix (2 days).

**Lower-priority, later (after Phases A-H land)**:
7. IR text round-trip parser (Level B IR serialization) — Stage 9 Phase L1 prerequisite.
8. Fix-point iteration in optimise() — IR plan §2.4.
9. Per-pass stats counters — `NIX_V3_OPT_STATS=1`.

## 7. The surprising finding (one-line)

**The FileCheck-style infrastructure ALREADY EXISTS in `ir_dump.cc` (lines 420-510) and is used in 25 places in smoke.cc.** The team built it for unit-testing hand-built IR but never connected it to a `.nix → IR → CHECK` end-to-end flow. Connecting it is the single highest-leverage near-term test-infrastructure win — modest engineering cost, large persistent payoff as IR Phases A-H land.

## 8. What this review does NOT cover

- Code quality of the `opt_*.cc` passes themselves (didn't audit).
- Correctness of any specific pass (assumes them correct unless tests say otherwise).
- Performance characteristics of each pass (no measurement).
- Comparison to other compilers' optimizer pipelines (GHC's Core, LLVM's `opt`, etc.).

These could be follow-up reviews if useful.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.
