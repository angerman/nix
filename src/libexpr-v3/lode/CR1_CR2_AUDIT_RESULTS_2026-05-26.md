# CR1 + CR2 audit results — 2026-05-26

**Date:** 2026-05-26 (afternoon)
**Author:** session synthesis (2 parallel Explore agents)
**Status:** active — actionable findings; R1 trigger evaluation
**Triggering context:** user-requested parallel audits per `ARCHITECTURE_CRITIQUE_2026-05-26.md` §8.1 (CR1 + CR2). Both run as read-only Explore agents in parallel with team code work.

Companion docs:
- [`ARCHITECTURE_CRITIQUE_2026-05-26.md`](ARCHITECTURE_CRITIQUE_2026-05-26.md) — the audit dispatching analysis
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) §6.5 — R1 trigger conditions
- [`RCA_815_CROSS_WORKLOAD_2026-05-25.md`](RCA_815_CROSS_WORKLOAD_2026-05-25.md) — #815 RCA + "Lessons for the Full variant"

---

## 1. Position (TL;DR)

**CR1 (lower.cc iteration-order audit): AR5 trigger FIRES.** 2 HIGH-risk + 1 MEDIUM-risk sites identified. **The Light variant pattern does NOT trivially generalize to these sites**: thunkify is scattered across arbitrary AST descent rather than localized to attrset construction. Agent proposes "post-lower FuncId renumbering pass" as the targeted fix (3-5 days), but at that cost, R1 Full de Bruijn IR (1 week) becomes comparable AND closes the broader non-determinism class structurally.

**CR2 (serialize cross-check): CR2 CLOSES.** 0 gaps found. Both `collectReferencedSymbols` and `remapSymbolsInBytecode` handle all 4 operand-level SymbolIds + 5 trailing-data patterns + 2 lambda-level paths symmetrically. Schema 12/13 `selectorSym` handling verified. AR24 from ARCHITECTURE_CRITIQUE §3.3 can be marked CLOSED.

**Recommended action:** the cost-benefit shifted concretely. R1 is now the recommended path (over Option B / Option C intermediate fixes). **Schedule R1 as a 1-week dedicated effort within the next 2 weeks.** Concurrently: land CR2's CI test (~30 LoC bash, ≤1 day).

---

## 2. CR1 findings — lower.cc iteration-order audit

### 2.1 Findings table

Agent searched `lower.cc` exhaustively for `m.functions.emplace_back()` (FuncId allocation) and iteration patterns over symbol-keyed containers. Cross-referenced with the Light variant pattern at `lower.cc:2728-2790` (`formalCanonIdx[c]` alphabetical-by-symbol).

| file:line | site type | what it builds | canonical-sort scope? | risk |
|---|---|---|---|---|
| `lower.cc:437` | FuncId allocation | `thunkifyRecAttrSelect` wrapper | NO — called from `resolveVarAttrSelect:418` triggered by AttrSelect on rec-scope var refs; no iteration guard | **HIGH** |
| `lower.cc:1103` | FuncId allocation | `lowerLambda` simple-arg entry | NO — baseline lambda lowering, one-shot per lambda expr | LOW |
| `lower.cc:1272` | FuncId allocation | Lambda formal-default thunk body | YES — inside `for (c = 0; c < nF; ++c)` with `i = formalCanonIdx[c]` (canonical sorted at lines 1258-1262) | DEFENDED |
| `lower.cc:1824` | FuncId allocation | generic `thunkify` wrapper | NO — called from `thunkifyForAttr`/`thunkifyForArg` at ~8 AST descent sites (lines 1806, 1815, 2415, 2486, 2512, 2571, 2635, 3291); no canonical sort | **HIGH** |
| `lower.cc:2791` | FuncId allocation | `lowerLetRecCapture` rec-attrset entries | YES — `for (c = 0; c < canonicalIdx.size(); ++c)` with entries from `twEntries[canonicalIdx[c]]`; canonicalIdx alphabetical (lines 2782-2786) | DEFENDED (Light variant) |
| `lower.cc:2960` | FuncId allocation | `pushInheritFromCache` from-expr thunk | NO — iterates `inheritFromExprs` by displacement index (positional), but nested `lowerExpr(fromE)` at line 2968 may allocate FuncIds whose order depends on parent symbol iteration | **MEDIUM** (conditional) |

### 2.2 The architectural insight (most important)

The agent surfaced a structural issue with the Light variant pattern: **it doesn't generalize to thunkify sites because they're scattered across arbitrary AST descent rather than localized to attrset construction**.

Specifically:
- `thunkifyRecAttrSelect` is called from arbitrary `AttrSelect` expressions throughout the source tree (every `x.foo` on a rec-scope binding)
- Generic `thunkify` is called from ~8 different AST descent sites (each thunk-wrapping a lazy attr / primop arg / list element / etc.)

For both, a canonical-sort-at-the-iteration-site fix doesn't apply — there's no iteration to sort, just downstream-DFS allocation of FuncIds in AST-descent order. **The Light variant pattern is a localized solution to a localized problem (attrset entries); these sites need a different approach.**

Agent's proposed Option B: **post-lower FuncId renumbering pass.** Sort all `(FuncId → name)` mappings alphabetically by name; emit a permutation table into the CU header; serialize-time remap; deserialize-time apply. This is the SAME pattern as the existing SymbolId sparse remap, applied to FuncIds. Closes the class for FuncId divergence without requiring per-site canonical iteration. Estimated effort: 3-5 days.

### 2.3 Verdict

**AR5 trigger FIRES.** Per the verdict rubric:
- 0-2 sites → CR1 closes, Light + lint sufficient
- 3-5 sites → AR5 fires; R1 within 2 weeks OR Option B (post-lower renumbering) in 3-5 days
- 6+ sites → R1 strongly recommended

Agent count: **2 HIGH + 1 MEDIUM = 3 sites in the trigger zone**.

**Caveat (mine, not agent's):** the HIGH classification is conditional. The thunkify sites only diverge across processes IF an upstream iteration is non-canonical. The Light variant fixed the two largest upstream sources (lowerLetRec + lowerAttrs). If the third leak the #815 RCA flagged ("140/162 files still diverge") is fully closed by the Light variant — which the team hasn't confirmed via re-running `V3_DBG_DESERIALIZE_VERIFY` post-fix — then thunkify sites become LOW-risk in practice.

**However:** the RCA's "140/162 still diverge" diagnostic was the Light Phase 3 measurement (before the Light variant landed fully). Whether THIS metric has been re-measured post-Light is unclear from the docs. **Worth checking before final R1 decision.**

### 2.4 Recommended action (CR1)

Two concrete sub-actions:

**(a) Verify whether Light variant actually closed all of #815's divergence** (≤ 1 day).
Re-run `V3_DBG_DESERIALIZE_VERIFY` on the 5-pkg sweep + haskell-nix-example reproducer (the original #815 trigger). Count remaining divergent files.
- If 0 divergent files: Light variant was sufficient; thunkify sites are LOW-risk in practice; CR1 closes with documentation.
- If > 0 divergent files: the third leak IS still in the tree; CR1 expands to find it; high probability AR5 fires definitively.

**(b) IF (a) confirms residual divergence OR if team prefers structural over tactical:** schedule R1 within 2 weeks. Effort: 1 week / ~300 LoC per `LINKING_DESIGN_2026-05-17.md` Phase L1.

**The cost-benefit shift:** Option B (post-lower FuncId renumbering, 3-5 days) was originally the "fast intermediate." But at 3-5 days for FuncIds-only + still needing to address SymbolId process-locality eventually + still needing AR22's opt_*.cc unordered_map fixes — the total intermediate-route cost is 5-8 days. R1's 1 week closes BOTH classes structurally. **R1 is now the recommended path.**

---

## 3. CR2 findings — serialize.cc cross-check

### 3.1 Findings

Agent built a complete opcode × `collectReferencedSymbols` × `remapSymbolsInBytecode` dispatch table.

| Opcode category | Specific opcodes | Coverage |
|---|---|---|
| Operand-level SymbolId | OP_ATTRS_SELECT, OP_ATTRS_HAS, OP_WITH_LOOKUP, OP_REC_BINDING_SLOT_REF | ✓ Both (collectReferencedSymbols:148-152, remapSymbolsInBytecode:387-405) |
| Trailing-data SymbolId (n entries × 2 words: name+pos) | OP_ATTRS_INIT, OP_ATTRS_INIT_DYN, OP_ATTRS_REC_INIT, OP_ATTRS_LET_REC_INIT, OP_ATTRS_REC_INIT_TAIL | ✓ Both (collect:158-179, remap:406-460); includes re-sort post-remap |
| LambdaDescriptor formals | per-formal `name` SymbolId | ✓ Both (collect:191-194, remap:837-839); re-sorted post-remap (849-854) |
| LambdaDescriptor.selectorSym | Schema 12 `selectorSym` field | ✓ Both (collect:195-199, deserialize:772, remap:856-863) |
| IC follow-up word | OP_REC_BINDING_SLOT_REF IC entry | ✓ Both skip symmetrically (collect:156-157, remap:405); IC is process-local; zeroed on load |
| OP_ATTRS_REC_SET slot operand | slot index (NOT SymbolId) | ✓ Asymmetric BY DESIGN — slot is index not symbol; patched via `pending` permutation stack from REC_INIT remap |

### 3.2 Verdict

**0 gaps. CR2 closes cleanly. AR24 from ARCHITECTURE_CRITIQUE §3.3 marked CLOSED.**

Both functions maintain symmetric coverage over all SymbolId-carrying operand paths. Schema bumps v9-v13 each introduced new fields correctly handled in both. The agent verified:
- Schema 9 (#781b sparse symbolTable) — dispatch mirrored
- Schema 10 (#779 OP_REC_BINDING_SLOT_REF IC) — IC follow-up skipped symmetrically
- Schema 12 (#814 selectorSym) — collect + remap + deserialize all present
- Schema 13 (#814 formal re-sort) — invariant restoration in place

### 3.3 Recommended action (CR2)

**Codify as CI test (~30 LoC bash, ≤ 1 day).** Per agent recommendation:

```bash
# test/lint-serialize-symbolid-coverage.sh (sketch)
# 1. Parse serialize.cc for opcodes appearing in collectReferencedSymbols switch/conditionals
# 2. Parse serialize.cc for opcodes appearing in remapSymbolsInBytecode switch/conditionals
# 3. Compare sets; error if any SymbolId-carrying opcode is missing from either
# 4. Wire into CI (pre-commit + lint suite per A4 lint precedent)
```

Plus a process rule: when adding a new opcode that carries SymbolId operands, the commit MUST add handling to BOTH `collectReferencedSymbols` AND `remapSymbolsInBytecode`, AND bump `kSchemaVersion`. This is the existing A4 lint extended to a related class.

The CI test catches FUTURE drift; the audit itself catches CURRENT state. Combined, the AR24 risk is structurally closed.

---

## 4. Updated R1 trigger status

Per NEXT_STEPS §6.5 R1, the trigger conditions are:

| Trigger | Status (pre-CR1) | Status (post-CR1) |
|---|---|---|
| Second #815-class bug | Not fired | Not fired (no new bug; audit found queued reproducer sites) |
| Stage 9 trigger B | Not active | Not active |
| Unison Item 3 active | Not active | Not active |
| Stage 13 active | Not active | Not active |
| AOT committed | Not active | Not active |
| **AR5 audit ≥ 3 sites** | **Unscheduled** | **FIRED (CR1: 2 HIGH + 1 MEDIUM = 3)** |
| Symbol-table remap regresses | Not active | Not active |

**R1 status change: from "trigger-PROBING" to "trigger-CONDITIONALLY-FIRED, awaiting verification of post-Light divergence state."**

The "conditional" qualifier reflects §2.3's caveat: the HIGH classification depends on whether upstream non-canonical iteration sites still exist post-Light variant. The 2.4(a) verification (re-run `V3_DBG_DESERIALIZE_VERIFY`) settles the question in ≤ 1 day.

**Recommended sequencing:**

1. **Today / tomorrow:** Run 2.4(a) verification (re-run V3_DBG_DESERIALIZE_VERIFY on 5-pkg sweep + HNE post-Light variant). ≤ 1 day.
2. **Decision point:** if residual divergence = 0 → CR1 verdict softens to "audit found queued reproducers but no active leak; defer R1 unless second #815-class bug fires"; if residual > 0 → R1 fires unconditionally.
3. **Concurrent:** land CR2's CI test (~30 LoC, ≤ 1 day) regardless of R1 decision.

---

## 5. Cost-benefit re-analysis

The agent's CR1 finding changes the cost-benefit shape for intermediate options:

| Path | Effort | Closes |
|---|---|---|
| ~~Option C (replace unordered_map with std::map in opt_*.cc)~~ | 2-3 days | opt_*.cc unordered_map class ONLY; does NOT address lower.cc thunkify class |
| Option B (post-lower FuncId renumbering pass) | 3-5 days | lower.cc thunkify class ONLY; does NOT address opt_*.cc unordered_map class |
| Option B + Option C combined | 5-8 days | Both classes tactically; does NOT eliminate SymbolId process-locality |
| **R1 Full de Bruijn IR (Phase L1)** | **~1 week** | **Both classes + SymbolId process-locality + enables R5/R8 prerequisites** |

**The intermediate-route total cost (5-8 days) is comparable to R1 (~1 week).** Plus R1 unlocks:
- Stage 9 dedup re-measurement at IR-subtree granularity (revival trigger B)
- Unison Item 3 + 4 prerequisites
- Stage 13 (parallel eval) prerequisite per AR10
- AOT distribution stability per AR8
- Elimination of Light-variant brittleness across all current AND future emit sites
- 10-30 % off cold-load wall via symbol-table size reduction

**Recommendation:** if R1 fires (per §4 verification), do R1 directly. Don't sequence Option B + Option C first — the cost is comparable and R1 closes both classes structurally.

---

## 6. Honest limits

- **CR1 agent classification may be conservative.** The HIGH-risk classification for thunkify sites is conditional on upstream non-canonical iteration. If §2.4(a) verification shows zero residual divergence, HIGH softens to LOW. **The audit alone doesn't settle R1 firing — verification does.**

- **CR2 audit is for the CURRENT state.** If new opcodes are added later without updating both functions, the gap re-opens. Hence the CI lint recommendation.

- **The agent did not read every opt_*.cc file** (that was Agent 2's scope in the prior dispatch). The 7 `unordered_map` sites in opt_*.cc (AR22) are SEPARATE from CR1's lower.cc findings and remain open per the prior critique.

- **Option B agent estimate of 3-5 days** is the agent's judgment, not validated. Could be 1 week in practice (cache header changes + remap + tests + cross-process verification).

- **R1 estimate of 1 week** is from `LINKING_DESIGN_2026-05-17.md` Phase L1, now over a week old. Re-estimation before commitment is warranted.

- **The §5 cost-benefit assumes R1 unlocks downstream items at zero additional cost.** In practice some integration work for R5/R8 is separate; R1 is necessary-but-not-sufficient for those.

- **The MEDIUM classification for `lower.cc:2960` pushInheritFromCache** is also conditional. The agent's reasoning is sound (nested lowerExpr could trigger non-canonical iteration), but in practice this fires only if `inherit (callPackage ...)` patterns are present AND the nested lowering touches non-canonical sites.

---

## 7. What this changes in NEXT_STEPS

If §2.4(a) verification confirms R1 needed:

1. **§3 Tier A** — add a new item:
   - **A5:** R1 Full de Bruijn IR (Phase L1). Effort: ~1 week. Justified by CR1 verdict + #815 reproducer verification.

2. **§6.5 Tier R** — update R1 status:
   - From "deferred per the RCA doc's 'Lessons' section"
   - To "FIRED 2026-05-26; promoted to Tier A as A5; see CR1_CR2_AUDIT_RESULTS_2026-05-26.md §4"

3. **§8.5 AR list** — close AR24:
   - From "Sparse symbol-table walk vs remap dispatch can drift on new opcode"
   - To "CLOSED 2026-05-26 (CR2 audit, 0 gaps); CI lint scheduled per CR2 §3.3"

4. **§12 Operating rules** — add:
   - "New opcode carrying SymbolId MUST update collectReferencedSymbols + remapSymbolsInBytecode + bump kSchemaVersion in same commit" — to be enforced by CR2 CI lint extension

5. **§14 Cross-references** — add this doc

If §2.4(a) verification shows zero residual divergence:

1. **§6.5 Tier R** — update R1 status to "trigger-CONDITIONAL; CR1 audit found queued reproducers but no active leak. Defer until second #815-class bug fires OR another trigger condition."

2. **§8.5** — close AR24 same as above

3. Land CR2 CI lint regardless

---

## 8. Cross-references

- [`ARCHITECTURE_CRITIQUE_2026-05-26.md`](ARCHITECTURE_CRITIQUE_2026-05-26.md) — CR1 + CR2 source; §3.3 AR24 closed by this doc
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) §6.5 (R1 trigger) — updated per §7 above
- [`RCA_815_CROSS_WORKLOAD_2026-05-25.md`](RCA_815_CROSS_WORKLOAD_2026-05-25.md) — "140/162 files still diverge" reference for §2.4(a) verification target
- [`LINKING_DESIGN_2026-05-17.md`](LINKING_DESIGN_2026-05-17.md) — R1 Phase L1 source spec
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §5.7 — §2.4(a) verification embodies the methodology-audit-before-structural-conclusion rule

**Commits referenced:**
- `93da764fd` Light Phase 1+2 canonical iteration in lowerLetRec + lowerAttrs
- `1b7496844` #815 truly resolved
- `781b9...` (#781b) Schema 9 sparse symbolTable
- `3a4b06ebc` (#779) Schema 10 OP_REC_BINDING_SLOT_REF IC
- `ed8fa0669` (#814) Schema 12/13 selectorSym + post-remap formal re-sort
- `46ce47c8a` A2 V3_RELEASE LANDED (parallel work; no interaction with CR1/CR2)
- `98ca953bb` ChainBindings scaffold (parallel work)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
