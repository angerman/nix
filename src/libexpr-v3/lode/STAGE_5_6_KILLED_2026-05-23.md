# Stages 5 + 6 Killed — 2026-05-23

Stages 5 (Hidden classes / attrset shapes) and 6 (Polymorphic Inline
Caches) were killed by Rule 0 at 07:27 yesterday-into-today, commit
`fe7c17498` (#778). The Phase L0 dispatch-budget spike showed
AttrSelect family at 2.24 % of dispatch on hello.drvPath — below the
roadmap's pre-committed 10 % kill threshold. Stage 6 was implicitly
killed because it was built on Stage 5's shape system.

This memo mirrors `STAGE_9_KILLED_2026-05-22.md` in structure. Two
stages, one falsifier, both gone. Cumulatively ~12 weeks of work
cancelled across the original roadmap (Stage 5: 6 wk; Stage 6: 6 wk).

## 1. What Stages 5 + 6 were

**Stage 5 (Hidden classes / shapes — Weeks 29-34 in original plan):**
- V8-style attrset shape interning
- Shapes carry attribute layout (which symbols, slot indices)
- `OP_ATTRS_SELECT` would dispatch on shape identity for fast attribute lookup
- Substrate for polymorphism specialisation
- Required ABT identity refactor (Unison Item 2) as prerequisite

**Stage 6 (Polymorphic Inline Caches — Weeks 35-40):**
- Per-call-site cache of (shape, offset) pairs
- Inline-cache hit path: shape match → direct offset use
- Miss path: shape lookup + cache update
- Built on Stage 5's shape system; couldn't exist without it

Combined estimate in `ROADMAP_TO_VISION`: ~12 weeks.

## 2. The kill criterion (committed in advance)

`ROADMAP_TO_VISION_2026-05-15.md` Stage 5 §Kill criterion:

> "AttrSelect family ≥ 10 % of dispatch on representative workloads.
> Below this, even a free PIC saves negligible wall time; redirect
> effort to whatever IS dominant in dispatch."

The kill criterion was specifically denominator-aware: it asked
"is this WORTH a multi-week design?" not "is this hot at all?"
10 % was the chosen floor for Stage 5 + 6's combined investment to
pay back.

## 3. The cheap-proxy mechanism (saved ~1 week vs full PIC prototype)

Same pattern as Stage 9's L0 spike: rather than implementing a PIC
prototype and measuring its delta (which would have required the
ABT refactor + shape system as prerequisites, ~3-4 weeks), the L0
spike measured the **upper bound on what a PIC could save** by
asking "what fraction of dispatch is AttrSelect?".

Implementation: extend `NIX_VM_OPCOUNTS=1` to also print an
"AttrSelect family share" line summing OP_ATTRS_SELECT +
OP_ATTRS_SELECT_DYN + OP_ATTRS_SELECT_IC + OP_ATTRS_HAS +
OP_ATTRS_HAS_DYN as a fraction of total dispatch. ~30 LoC change
in `run.cc` and `vm.cc`.

## 4. The measurement

hello.drvPath full eval:

```
total dispatch              : 15 000 349
OP_GET_LOCAL                 : 19.32 %
OP_GET_UPVALUE               : 15.52 %
OP_SET_LOCAL                 : 14.08 %
OP_REC_BINDING_SLOT_REF      :  9.05 %
OP_RETURN                    :  5.16 %
OP_MAKE_THUNK                :  4.41 %
OP_CALL                      :  3.69 %
OP_ATTRS_REC_SET             :  3.63 %
OP_BRANCH_FALSE              :  2.54 %
OP_TAIL_CALL                 :  2.26 %
OP_MAKE_CLOSURE              :  1.81 %
OP_FORCE                     :  1.66 %
─────────────────────────────────────
AttrSelect family            :  2.24 %  ← Stage 5/6 decision input
```

**AttrSelect family: 2.24 %.** Kill threshold: ≥ 10 %. Decisively
below.

Wall-clock ceiling argument (denominator-independent): even if a PIC
made every AttrSelect dispatch free (which it can't — the hit path
still pays cache probe + offset use), the wall-clock ceiling on
hello.drvPath is ~26 ms (2.24 % × 1.165 s). Multi-week design +
implementation can't justify a 26 ms wall saving.

## 5. Where the dispatch budget actually goes

The same #778 measurement re-shapes the project's next perf lever:

| Category | % of dispatch | Lever |
|---|---|---|
| Local-stack motion (GET_LOCAL + SET_LOCAL + GET_UPVALUE) | **48.92 %** | Different VM shape (register-based, super-instructions, threaded code, ABT/closure-form) |
| Slot reference + side-table (OP_REC_BINDING_SLOT_REF) | 9.05 % | Possibly inline-cache (#779 Schema 10 attempted this) |
| Return + frame management (OP_RETURN, OP_MAKE_THUNK, OP_CALL, OP_TAIL_CALL, OP_MAKE_CLOSURE) | 17.33 % | Stage 4 strictness reduces frame count; Stage 7 selector thunks bypass some frames |
| AttrSelect family | 2.24 % | (killed — not worth optimising in isolation) |
| Other | 22.46 % | Mix of literals, branches, force, etc. |

The biggest single lever is **local-stack motion at 48.92 %**. This
is what Stages 5 + 6's calendar should be reallocated to address.
The fix shape is a different VM shape, not PICs.

## 6. What this kills (Rule 0)

**FALSIFIES Stage 5's hidden-classes-for-AttrSelect hypothesis.** Even
if shape identity were perfectly captured (Stage 5) AND inline caches
hit 100 % (Stage 6), the maximum wall-time win is ~26 ms on
hello.drvPath. Below the threshold that justifies multi-week
architectural investment.

**IMPLICITLY KILLS Stage 6 (PICs).** Stage 6 was built on Stage 5's
shape system — PIC cache entries are keyed on shape identity. Without
shapes, PICs have no key. Even if PICs were redesigned to work
without shapes (caching at the call-site without shape identity), the
same wall-clock ceiling argument applies: 2.24 % of dispatch is the
ceiling for ANY OP_ATTRS_SELECT optimisation, not just shape-based
ones.

## 7. Revival conditions

The kill is robust against most foreseeable changes, but not all.
Specific re-measurement triggers:

### 7.1 Trigger A — denominator shift after VM-shape change

**Hypothesis**: if local-stack motion (currently 48.92 %) drops
significantly via register-VM, super-instructions, or threaded code,
the remaining 51 % redistributes. AttrSelect % may rise.

**Re-measurement**: re-run `NIX_VM_OPCOUNTS=1` + the
AttrSelect-family banner on hello.drvPath AND cardano-node M5.

**Trigger**: any major VM-shape change lands (target candidate is the
local-stack-motion fix tracked in the post-Stage-9 calendar).

**Effort**: 1 day (re-run, compare).

**Decision rule**:
- If AttrSelect ≥ 10 % of dispatch on the new VM shape: re-open
  Stage 5 design.
- If AttrSelect 5-10 %: do a single-day Stage 5 design spike to
  confirm whether the wall-clock ceiling crosses a worth-implementing
  threshold (target: ≥ 5 % wall-time savings on benched workloads).
- If AttrSelect < 5 %: kill stands; redirect any "we should add PICs"
  discussion back to this memo.

### 7.2 Trigger B — different workload character

**Hypothesis**: some Nix workloads (sustained dictionary lookups,
heavy `lib.lookupAttr` chains, deeply-nested attr-path access) might
have AttrSelect-heavier profiles than hello.drvPath / cardano-node M5.

**Re-measurement**: when a user reports a specific workload showing
slower-than-expected v3 performance AND profiling identifies AttrSelect
as a hot category.

**Trigger**: user report + profile data.

**Effort**: workload-specific.

**Decision rule**: same as Trigger A.

### 7.3 Trigger C — monomorphic call-site density

**Hypothesis**: PICs specifically (not Stage 5's shape system) might
pay if a future analysis shows that AttrSelect call-sites are highly
monomorphic — i.e., the same site sees the same shape almost always.
In that case PIC hits would be near 100 % and bypass even shape
lookup.

**Re-measurement**: add a `NIX_V3_CALLSITE_SHAPES=1` diagnostic that
records per-call-site shape distribution; analyse for monomorphic
clustering.

**Trigger**: someone is investigating attrset performance and wants
to falsify or confirm this independently of Trigger A/B.

**Effort**: 2-3 days (instrument, run, analyse).

**Decision rule**: if ≥ 80 % of AttrSelect dispatch hits monomorphic
call-sites (single shape) AND wall-time savings would exceed 3 %, open
a narrower Stage 6 (PIC-only, no hidden-class system) design spike.

### 7.4 Probability assessment

| Trigger | Probability of firing | Probability of revival after firing |
|---|---|---|
| A (denominator shift) | Moderate (if local-stack fix lands) | Low (would need ~5× redistribution, unlikely) |
| B (different workload) | Low-moderate (depends on user workload mix) | Low (hello + cardano were broad samples) |
| C (monomorphic clustering) | Speculative | Low-moderate (PIC-only might still pay if hit rate is high) |

**Honest read**: revival is unlikely in any of the three paths
currently visible. The wall-clock ceiling argument from §4 is
robust — 26 ms of optimisable surface, even after redistribution,
is not multi-week-justifying. The kill is durable.

## 8. Consequences (work cancelled / demoted)

- **#773 (ABT alpha-equivalent identity refactor)** was a Stage 5
  Phase prereq. After Stage 9 killed it as a Stage-9 prereq, it had
  no remaining live consumer in the perf-stage path. With Stage 5
  also killed, ABT is **dormant** for the perf path entirely.
  However: ABT remains live for **non-perf** consumers (Unison
  Item 3 — hash-keyed eval cache; Unison Item 4 — effect propagation;
  v3-lint Mode 2 IR-level rules). It's dormant pending one of those
  consumers becoming a project priority.
- **Stage 6 (PICs) cancelled.** ~6 weeks reclaimed.
- **Stage 5 (Hidden classes) cancelled.** ~6 weeks reclaimed.
- **Cumulative reclaim from Stage 5 + 6 + 9**: ~17 weeks.
- **`ROADMAP_TO_VISION` needs updating** — Stages 5 + 6 rows removed
  from stage list and Effort Summary; cross-references pruned.
- **End-state target reshaped.** The original "v3 at end of Stage 7
  + Stage 14" assumed Stages 5 + 6 closed. With them dead, the
  end-state perf target depends on:
  - Stage 4 strictness wins (currently 0 elisions on Force(MkThunk)
    variant; v4.4 cross-fn through higher-order callees pending)
  - Stage 7 (selector thunks) — was W41-W44, now shifts left
  - **The local-stack-motion fix** — the new biggest single lever,
    a yet-to-be-designed stage targeting register-VM /
    super-instructions / threaded code

## 9. Lessons (Rule 0 working again)

Second cleanest Rule 0 outcome to date (after Stage 9's:

1. **Pre-committed kill criterion** in the roadmap (≥ 10 %), not
   added post-hoc.
2. **Cheap proxy** measured the upper bound on what optimisation
   could save (the dispatch %), avoiding the cost of implementing
   the optimisation just to measure it.
3. **Two-workload validation** (hello.drvPath + cardano-node M5)
   ruled out workload-specific quirks.
4. **Binary decision**, executed immediately.
5. **The measurement revealed the next lever** — 48.92 % local-stack
   motion — without which the team would have spent ~12 weeks on
   Stage 5 + 6 and emerged still ~30× slower because the actual
   bottleneck is elsewhere.

This is exactly what the falsification rule
(`feedback_falsification_rule.md`) is for. Compare against a
hypothetical where Stage 5 + 6 had been attempted: 12 weeks + ABT
refactor + shape system + PIC implementation = 3 months of work
before discovering the wall-clock ceiling argument made it
unwinnable.

## 10. References

- Falsification rule: `feedback_falsification_rule.md`
- Original Stage 5 + 6 design hooks: `ROADMAP_TO_VISION_2026-05-15.md`
  Stages 5 + 6 sections (should be marked SUPERSEDED)
- Stage 5 kill criterion: `ROADMAP_TO_VISION_2026-05-15.md` Stage 5
  §Kill criterion (line 305-308)
- Sibling kill (same pattern, prior day): `STAGE_9_KILLED_2026-05-22.md`
- Architectural review providing the Hölzle/Ungar 1991 + JSC pre-DFG
  interpreter PIC ceiling reference (~2-3× ceiling already noted in
  roadmap line 799): `project_architectural_review_2026-05-17.md`
- ABT refactor status (now fully dormant): `UNISON_IDEAS_2026-05-07.md`
  §2 (Unison Item 2)
- Local-stack-motion lever (the new biggest lever this measurement
  revealed): see `ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md` §Concrete
  plan for the candidate VM-shape change
- Commit hash: `fe7c17498` (2026-05-23 07:27)

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
