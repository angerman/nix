# V3 Perf Methodology Arc — 2026-05-23 mega-session

## Purpose

This document captures the methodology corrections + measurement
infrastructure + ship/revert cycles across the long 2026-05-23
mega-session.  The PERF_AUDIT_2026-05-23 review at the start of
the resumption flagged that #780/#783 "FALSIFIED" memos were
estimate-based, not implementation-tested.  This document is the
durable artifact of the rigor cycle that followed.

## Commits (chronological)

| commit | subject | category |
|---|---|---|
| 14c8da998 | LAND OP_SET_LOCAL_KEEP per audit T2.1 | implementation |
| 6ce43b486 | REVERT OP_SET_LOCAL_KEEP — A/B +2.8 ± 5 ms within σ | revert-with-data |
| 3b0e567ed | OPCYCLES per-op observed cycles (initial) | measurement infra |
| 36233450b | OP_RETURN breakdown — case body is 5% not 51% (RCA) | RCA |
| 150f356dd | OPCYCLES inter-dispatch-loop fix (#790) | methodology fix |
| 308002951 | Per-primop wall-clock (#788) | measurement infra |
| 37d07a1c2 | LAND __derivCoerce outPath-String fast path | implementation |
| 9c9eea6e2 | REVERT __derivCoerce shortcut — A/B -4.8 ± 7 ms < 8 ms ship | revert-with-data |
| ca45020e9 | Symbolise OP_ATTRS_UPDATE_TAIL + OP_IFD_PROBE (#789) | diagnostic |

Total: 9 commits in this rigor-correction subsession.  Plus the
earlier session's wall-clock wins (#777, #779, #781b) and
falsifiers (#772, #775, #778) covered separately.

## The methodology corrections

### Three categories of bugs caught

  1. **(Reviewer caught)** Estimate-based "FALSIFIED" memos.
     #780, #783 used assumed per-op-ns multiplied by counts.
     If the assumption was wrong, the conclusion direction
     could flip.  Fix: build OPCYCLES (observed per-op
     cycles).

  2. **(Self caught)** OPCYCLES misattributes outer-C++ work
     across dispatch-loop exits.  When OP_RETURN exited a
     dispatch loop, the outer C++ continuation + inter-loop
     transition got credited to OP_RETURN, giving it a
     fictitious "51% of eval" headline.  Fix: file-scope
     thread_local + save/restore in dispatchLoop's VMScope.

  3. **(Self caught)** Call count ≠ wall share.  The existing
     primop-call-count printout suggested `elem` (158K calls)
     was hot; per-primop wall-clock showed `elem` is 0.04%
     of wall (123 ns/call).  Real hot primops were derivation
     primops, called only ~400 times but at 4-12 ms each.
     Fix: extend primOpCounter with `nanos` accumulator.

### The implement-then-revert discipline

Two cycles, two distinct revert outcomes:

  * **#785 OP_SET_LOCAL_KEEP**: A/B delta +2.8 ± 5 ms — within
    noise floor.  Revert outcome (1): "doesn't help".
  * **#791 __derivCoerce shortcut**: A/B delta -4.8 ± 7 ms —
    positive but below ≥ 8 ms ship threshold.  Revert
    outcome (2): "helps but lever too small".

Both are valid Rule 0 falsifiers with different next-step
implications.

Both implementation commits stay in history (resurrect via
`git revert` if future use case wants the code).  The revert
commits carry the measurement data.

## Measurement infrastructure available (post-session)

| env var | what | gate cost |
|---|---|---|
| `NIX_VM_OPCOUNTS=1` | per-op dispatch count | ~1 ns/op |
| `NIX_VM_BIGRAMS=1` | (prev, curr) pair count | ~3 ns/op |
| `NIX_VM_OPCYCLES=1` | observed per-op ns (INCLUSIVE with #790) | ~20 ns/op |
| `NIX_V3_DBG_RETURN_BREAKDOWN=1` | OP_RETURN case-body phase breakdown | ~60 ns/RETURN |
| `NIX_VM_PRIMOP_TIME=1` | per-primop wall-clock (by name) | ~20 ns/primop |
| `V3_DBG_DESERIALIZE=1` | deserializeCU per-section timing | ~10 ns/section |
| `NIX_VM_STATS=1` | alloc + sizing breakdowns + RSS decomp | trace dump only |

Cross-validation: any two of these should agree on hot spots
within instrumentation noise.  When they disagree, suspect
attribution bug (see #787 RCA pattern).

## Current state (hello.drvPath, post-session)

  * Wall: 800.1 ± 2.1 ms (σ 0.3%; very stable)
  * v3:TW: ~1.45× (TW ≈ 550 ms)
  * Eval phase: ~750 ms (after subtracting ~50 ms disk-cache)
  * Total dispatch count: 15 000 349

### Where the wall ACTUALLY goes (corrected attribution)

The most-defensible breakdown (avoiding inclusive double-counting):

  * **Non-nesting opcodes** (their INCLUSIVE = SELF):
    - Stack motion (GET_LOCAL+SET_LOCAL+GET_UPVALUE): ~144 ms
    - OP_ATTRS_UPDATE_TAIL: ~90 ms
    - Other cheap leaf opcodes: ~30 ms
    - **Subtotal: ~265 ms (33% of wall)**

  * **Nesting opcodes** (INCLUSIVE inflated by nested forces):
    The triggered work is ~99% in 3 derivation primops:
    - `__derivationFromPreprocessed`: 4.8 s INCLUSIVE (405 × 11.9M ns)
    - `__derivCoerce`: 3.5 s (15K × 227K ns)
    - `__derivationStrictRaw`: 1.6 s (380 × 4.16M ns)
    - **Real wall in derivation construction (after de-duping nested): ~500 ms (62%)**

  * **Remainder** (GC, allocator, Phase D barriers, etc.):
    - ~35 ms (5% of wall)

## What's been tried + falsified

| candidate | falsification mechanism |
|---|---|
| #772 Stage 9 cell dedup | 1-day bytecode-dedup spike (fn_dedup_lb = 1.18×; <2× kill) |
| #775 Force(MkThunk) elision | Implementation showed 0/10 563 candidates passed |
| #778 Stage 5 AttrSelect PIC | Opcount: 2.24% of dispatch; <10% kill |
| #780 Register VM (multi-month) | Pre-fix OPCYCLES estimate 10-15% wall; post-fix shows even smaller. NOT justified. |
| #783 super-instructions (SET_LOCAL_KEEP) | A/B +2.8 ± 5 ms < 8 ms ship |
| #791 __derivCoerce outPath shortcut | A/B -4.8 ± 7 ms < 8 ms ship |

## What's NOT been tried (next-session candidates)

| candidate | est cost | est wall savings |
|---|---|---|
| #741 IFD content-addressed eval-result cache | 1-2 weeks | could halve derivation work (~250 ms wall) |
| #792 audit `__derivationStrictRaw` body | 2-3 days | unknown; needs investigation |
| Workload diversification (NixOS, cardano-node) | 1 day | data only — may shift hot levers |
| #779 OP_GET_REC_SLOT (already shipped) | DONE | -24% sweep |
| #781b sparse symbolTable (already shipped) | DONE | -23% wall |
| #777 zero-copy strv (already shipped) | DONE | -119 ms deserialize |

## Operating rules codified this session

1. **Falsification needs implementation.** Estimated per-op-ns
   multiplied by counts is a HYPOTHESIS, not a falsifier.
   Build the thing + observe.

2. **Observation infrastructure itself needs validation.**
   Cross-check tool A's headline with independent tool B.
   When they disagree, suspect attribution bug.

3. **Cross-validate hot-spot identifications via TWO metrics.**
   Call count alone is misleading; OPCYCLES inclusive is
   opcode-level; per-primop wall-clock is primop-level.
   Need both before deciding optimisation targets.

4. **Implement-then-revert IS a valid Rule 0 falsifier.**
   Two outcomes: (1) "doesn't help" (no measurable effect);
   (2) "helps but lever too small" (positive but below
   ship threshold).  Both reasons to revert; different
   consequences for follow-on work.

5. **Inclusive accounting double-counts; sum is meaningless.**
   Use INCLUSIVE for PRIORITISATION (which opcodes' work
   matters); use SELF for non-nesting opcodes' absolute wall;
   use per-primop for primop-level breakdown.

## When to revisit the falsified hypotheses

  * **Register VM (#780)**: if a JIT path is added (native code
    has ~1 ns/op dispatch, completely changes the per-op cost
    basis).
  * **Super-instructions (#783, #791)**: if a workload shows
    bigram top-1 > 15% with fusible subset > 60%, the saving
    may be material.
  * **Stage 5 PIC**: if AttrSelect-family share > 10% on some
    workload.
  * **Stage 9 cell dedup**: if a workload shows fn_dedup_lb > 5×.

## Cross-references

  * `lode/PERF_AUDIT_2026-05-23.md` — reviewer's audit that
    started this rigor cycle.
  * `memory/project_perf_audit_2026-05-23.md` — audit memo.
  * `memory/project_780_register_vm_falsified_2026-05-23.md` —
    RETRACTED + corrected.
  * `memory/project_783_super_inst_falsified_2026-05-23.md` —
    RETRACTED + corrected.
  * `memory/project_786_opcycles_2026-05-23.md` — OPCYCLES
    initial measurement.
  * `memory/project_787_opreturn_misattribution_2026-05-23.md` —
    self-caught OPCYCLES bug.
  * `memory/project_788_per_primop_2026-05-23.md` — per-primop
    headline finding.
  * `memory/project_791_derivcoerce_2026-05-23.md` — second
    implement-then-revert cycle.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.
