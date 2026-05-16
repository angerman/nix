# #558 Architectural Cleanup Arc — Progress Audit (2026-05-11)

Auditor: Claude (cross-checked against code).
Scope: the 57 commits in `git log --grep="558" src/libexpr-v3/`,
spanning Phases 1.5 → 4 (and the multi-week STG ladder that preceded
them).  All Phase 1/2/3/4 work landed today, **2026-05-11**, between
11:12 and 16:45 local time — the lode docs dated `2026-05-12` are
forward-dated by ~12h.

---

## Summary

- **The arc.**  v3 had an STG WHNF "recovery" workaround whose
  per-thunk side-table (`partialBindingsRegistry`) was the architectural
  root of multiple recurring bugs (#455 / #496 / #498 / #546 / #548 /
  #558 itself).  Today's arc replaces it with two complementary
  pieces: (a) blanket inherit-from thunkification matching TW's
  `from->maybeThunk` (eliminates the eager-force cycles the registry
  was masking), and (b) a per-thunk `shapeCell` heap pointer for the
  legitimate-inner-cycle cases (STG-correct per-thunk cell update).
- **Where the work landed.**  Phase 1.5 added `Thunk::shapeCell` and
  STG-correct innermost-only writes (commits `20adafe31`, `0677e7cd8`,
  `ead3f33ae`).  Phase 3.1–3.3g unified the kill-switch, flipped
  defaults, and **deleted** the registry + its consumers (~600 LOC,
  commits `77219c046` → `07a6352c1`).  Phase 4 cleanup added
  fakeClo pooling, path compression, and iterative spine walks
  (`34a59442e` → `ba2476e73`).
- **What this morning's audit flagged (S2/S3/S5).**  The triad —
  `OP_ATTRS_REC_INIT` publishes everything (S3), STG WHNF recovery
  returns approximate partial Bindings (S2), inherit-from lowered
  eagerly (S5) — is addressed in code: the publish code path is
  deleted, the STG WHNF chain consult is deleted, and S5's blanket
  thunkify (`NIX_V3_INHERIT_FROM_THUNK_ALL`) is now default-on.
  Architecturally a real fix.
- **Where it lands short.**  Cell-update-everywhere itself is still
  gated `NIX_V3_CELL_EVERYWHERE=1` **opt-in** in five places: the
  three write sites (REC_INIT, REC_INIT_TAIL, UPDATE_TAIL), the read
  site in `forceValue`'s Black branch, and the allocation site in
  `allocThunkSuspended`.  In the **default** build today, neither
  the registry (deleted) nor shapeCell (off) provides any
  mid-construction state visibility — STG's local-cycle semantics
  apply unconditionally, propped up entirely by THUNK_ALL's
  laziness.
- **The opt-out is missing.**  Commit `16d0adc81` advertised
  `NIX_V3_KEEP_PARTIAL_BINDINGS=1` as the escape hatch back to the
  legacy mechanism.  Phase 3.3 deleted the registry outright, so the
  flag is now dead — `grep` confirms zero references in source.
  There is no opt-back path if a workload regresses.
- **Known residual failures.**  Per
  `CELL_UPDATE_EVERYWHERE_2026-05-12.md` §"Phase 2 perf
  investigation": `(import nixpkgs {}).hello.name` **times out at
  120s+ under THUNK_ALL** on full nixpkgs (vs. TW's 0.7s), and
  v3-direct nixpkgs still produces a real cycle that shapeCell
  cannot mask.  The architecture is sound, the perf is not yet
  there, and v3-direct nixpkgs has an unresolved residual eval-order
  issue.

---

## What was deleted

| Commit | Phase | Deletion |
|---|---|---|
| `0fde92c28` | 3.3a | `finalizedBindings()` set + `compactPartialBindingsRegistry()` + lazy-cleanup machinery + finalized-skip checks |
| `db74e8544` | 3.3b | `publishToNearestBlackThunkFrame` + `publishToAllThunkFrames` + `partialBindingsDisabled()` gate — **447-line file delta** |
| `4d42bc200` | 3.3c | `withLookup` partial-Bindings consumer + OP_RETURN registry housekeeping |
| `a7792bc10` | 3.3d | `OP_ATTRS_SELECT` partial-Bindings peek (pre-force) |
| `121ce7835` | 3.3e | `OP_FORCE` STG WHNF deferral + `OP_ATTRS_SELECT` registry-wide chain peek |
| `5a67f4e6f` | 3.3f | OP_ATTRS_UPDATE/UPDATE_TAIL collapseDeferred, OP_REC_BINDING_SLOT_REF tolerant-force / last-ditch retry, `clearBlackMarksOnException` registry erase, pre-throw recovery, **the 200-line STG WHNF recovery** with TAINT + largest-layer + V3_DBG diagnostics |
| `07a6352c1` | 3.3g | `partialBindingsRegistry` (thread_local map) + `pickLargestLayer()` + `lookupInPartialChain()` + `PartialBindingsChain` typedef + `gc.cc` scavenger forward decl |

Net deletion: ~600 LOC of registry + consumer code.

**Rationale** (from commit messages): "Cell-update-everywhere via
`Thunk::shapeCell` (Phase 1.5) is now the ONLY partial-state
visibility mechanism in v3.  THUNK_ALL is the load-bearing
correctness fix; shapeCell handles legitimate inner [cycles]"
(`07a6352c1`).

---

## What replaced it

### 1. `Thunk::shapeCell` — separate cell for in-progress shape
- Defined at `include/v3/closure.hh:131-156`.  Distinct from the
  earlier STG-8 `cell` (which is the parent-entry-slot pointer for
  in-place updates on completion).
- Allocated at `include/v3/alloc.hh:360-374` —
  `allocThunkSuspended` allocates `Value * sc = allocValue()` and
  initialises `*sc = Tag::Thunk(t)` as the "pre-body" sentinel.
- Gated by `NIX_V3_CELL_EVERYWHERE=1` cached static in three
  separate places (`alloc.hh:365`, `vm.cc:4586`, `vm.cc:7502`).
- **All five reads/writes are still `__builtin_expect(s_cellEverywhere, 0)` cold paths**:
  - `vm.cc:4585-4598` — write at `OP_ATTRS_REC_INIT`
  - `vm.cc:4692-4705` — write at `OP_ATTRS_REC_INIT_TAIL`
  - `vm.cc:5367-5380` — write at `OP_ATTRS_UPDATE_TAIL`
  - `vm.cc:7502-7521` — read in `forceValue` Black branch (BEFORE
    legacy STG WHNF; the legacy block is now gone but the comment
    "Phase 3.3: STG WHNF recovery via partial-Bindings retired" sits
    immediately after)
  - `vm.cc:3598-3605` — write at OP_RETURN (final value writeback +
    clear)

All shapeCell writes are **innermost-THUNK_RETURN-only** by design
(commit `ead3f33ae` reverted the cross-thunk propagation hack
`52eb8f261` after it was found to recreate the same cross-thunk
pollution shape as the registry).  Each thunk's shapeCell holds
only its own body's in-progress state.

### 2. `NIX_V3_INHERIT_FROM_THUNK_ALL` default-on
- Flipped in commit `16d0adc81` (Phase 3.2); now opt-OUT via
  `NIX_V3_NO_INHERIT_FROM_THUNK_ALL=1`.
- Implemented at `lower.cc:1752-1755`:
  `s_thunkifyAll = !std::getenv("NIX_V3_NO_INHERIT_FROM_THUNK_ALL")`.
- Effect: every inherit-from from-expr is unconditionally lazy,
  matching TW's `from->maybeThunk(state, up)`.  Eliminates the
  eager-force cycles in inherit-from that the registry was masking.
- **This is the load-bearing correctness fix.**  With THUNK_ALL on
  AND the registry deleted, the libsForQt5 emit-order cycle stays
  closed (`run-558-emit-order-tests.sh` 4/4 OK).

### 3. Phase 4 perf scaffolding (not architectural, but related)
- Thread-local fakeClo pool (`34a59442e`): per-Suspended-force
  Closure allocations recycle through a thread-local pool (bucketed
  by `nUpvalues`, 0..15, cap 128/bucket).  Opt-out:
  `NIX_V3_NO_CLOSURE_POOL=1`.
- Path compression in `forceValue` and `OP_FORCE` chase loops
  (`b4b2c2a72`, `dfde0e33f`): on Tag::Thunk(Evaluated) →
  Tag::Thunk(Evaluated) chains up to 16, write the resolved WHNF
  back into each thunk's `evaluated` slot.  Opt-out:
  `NIX_V3_NO_PATH_COMPRESS=1`.
- Iterative App spine walk in `forceValue` (`ba2476e73`): mirrors
  the OP_FORCE pattern — `rights` buffer + leaf force, instead of
  recursive `forceValue(vm, left); callClosure(...)`.

These don't address the audit's findings but are aimed at THUNK_ALL's
nixpkgs-scale perf gap (110000 forces over 416278 thunks allocated
per `CELL_UPDATE_EVERYWHERE_2026-05-12.md:196`).

---

## Audit cross-reference (S2 / S3 / S5)

This morning's `COMPREHENSIVE_REPORT.md` flagged the
`OP_ATTRS_REC_INIT`-publishes-everything (S3) + STG-WHNF-recovery
(S2) + chain-peek triad as "the architectural root of the
#455/#496/#498/#546/#548 family" and the inherit-from from-expr
laziness gap (S5) as the eager-side enabler.

| Finding | Today's status |
|---|---|
| **S3** (REC_INIT publishes to ALL non-empty attrsets, vm:5060 etc.) | **FIXED.** `publishToNearestBlackThunkFrame` and `publishToAllThunkFrames` are deleted at `db74e8544`.  Bytecode opcodes `OP_ATTRS_REC_INIT_TAIL` (0x87) and `OP_ATTRS_UPDATE_TAIL` (0x88) still exist and are still emitted, but their runtime handlers no longer publish — only the cell-everywhere-gated shapeCell update remains.  See **Risks** for the dead-but-decoded opcode concern. |
| **S2** (STG WHNF recovery returns `pickLargestLayer(chain)`) | **FIXED.** Recovery code deleted at `5a67f4e6f`; the registry that fed it is deleted at `07a6352c1`.  The doc comment at `vm.cc:7522` reads: "Phase 3.3: STG WHNF recovery via partial-Bindings retired.  Local cycle falls through to BlackholeError throw."  STG-correct behavior on local cycles is now the default. |
| **S5** (inherit-from from-exprs not unconditionally thunkified) | **FIXED in default mode.** `NIX_V3_INHERIT_FROM_THUNK_ALL` flipped default-on (`16d0adc81`).  The narrow self-dot heuristic and `NIX_V3_SELF_DOT_MAX_LEVEL=4` workarounds still exist as opt-in fallbacks but are no longer the primary mechanism. |
| **Audit's P3 recommendation** ("When/if v3 transitions to cell-update-everywhere, the partial-bindings registry can be retired") | **PARTIALLY FULFILLED.** Registry is retired; cell-update-everywhere is in place but **opt-in only**.  In default mode, neither mechanism provides mid-construction state visibility — laziness via THUNK_ALL is the sole correctness guard. |

So in code-evidence terms, the morning's audit's biggest single
architectural finding is now fixed at the level of "the registry
and its consumers are deleted".  The deeper recommendation —
"transition to cell-update-everywhere" — is implemented as a flag
but **not turned on**.

---

## Open / partial

### A. `NIX_V3_CELL_EVERYWHERE` is opt-in only
**File:line cites:** `include/v3/alloc.hh:365`, `vm.cc:4586`,
`vm.cc:4693`, `vm.cc:5368`, `vm.cc:7502`.

All five `s_cellEverywhere` checks are `__builtin_expect(...,
0)` — i.e., the compiler is told to assume the slow path is
taken.  In default builds, `shapeCell` is never allocated and
never read.  The CELL_UPDATE_EVERYWHERE doc (line 153) says
"Gated by NIX_V3_CELL_EVERYWHERE=1 for safe rollout".  No
commit since flips this default.

**Verdict:** The architectural replacement mechanism exists but is
not active.  Today's default-mode v3 relies entirely on THUNK_ALL +
STG-correct-cycle-throws.  This is fine for local cycles (which
should throw) but means there is **no escape valve** for the
legitimate-inner-cycle cases shapeCell was designed to handle.

### B. `NIX_V3_KEEP_PARTIAL_BINDINGS` opt-out is dead
**File:line cites:** advertised in commit `16d0adc81` ("Opt back via
NIX_V3_KEEP_PARTIAL_BINDINGS=1") and in `vm.cc:7502` neighbourhood
comments.  `grep -r NIX_V3_KEEP_PARTIAL_BINDINGS src/libexpr-v3` →
**zero source matches** after Phase 3.3.

**Verdict:** The escape hatch documented in commit messages doesn't
exist in source.  If a user workload regresses under the
default-on flip, the documented fallback won't help.

### C. Dead bytecode opcode behaviour
**File:line cites:** `vm.cc:4602-4707` (OP_ATTRS_LET_REC_INIT,
OP_ATTRS_REC_INIT_TAIL), `vm.cc:5313-5383` (OP_ATTRS_UPDATE_TAIL);
emitter still distinguishes at `emit.cc:797`, `emit.cc:945`.

The TAIL variants (`OP_ATTRS_REC_INIT_TAIL`,
`OP_ATTRS_UPDATE_TAIL`) and `OP_ATTRS_LET_REC_INIT` exist
specifically to **distinguish** publishing behavior at the
runtime.  With publishing deleted, they differ from their non-TAIL
peers only in the (cell-everywhere-gated) shapeCell update.  Their
docstrings at `bytecode.hh:225-274` still describe the deleted
publish semantics — comments now lie about runtime behavior.

**Verdict:** ~3 opcode bytes (0x86, 0x87, 0x88) are still consumed
by distinct dispatch arms; their effective semantics have collapsed.
Mechanical opcode-merger cleanup deferred (per
`CLEANUP_AUDIT_2026-05-09.md` Tier 1: hold opcode deletions until
v5 disk-cache schema bump).

### D. `CFF_TAINTED` is now read-only
**File:line cites:** `include/v3/vm.hh:55` (declared);
`vm.cc:3573` (the only read); zero writers.

The TAINT bit was set by STG WHNF recovery to prevent memoization of
approximated results.  STG WHNF recovery is gone; CFF_TAINTED is
now a dead flag with a dead branch in OP_RETURN.  Comment at
`vm.cc:3558` still describes the flag's old role.

**Verdict:** Dead code; safe to delete in a follow-up.

### E. `NIX_V3_NO_STG_WHNF` env var is now stale
**File:line cites:** `vm.cc:7462` mentions it as a "bisect kill
switch" in a comment, but no `getenv("NIX_V3_NO_STG_WHNF")` call
remains in vm.cc — the STG WHNF block it gated is deleted.

**Verdict:** Documented-but-non-functional flag.  Add to the dead
flag inventory.

### F. v3-direct on nixpkgs still fails
**Reference:** `CELL_UPDATE_EVERYWHERE_2026-05-12.md:154` — "v3-direct
nixpkgs still fails the same way as before — shapeCell recovery
doesn't fire for the outer x thunk because x's body doesn't
directly fire OP_ATTRS_REC_INIT (its body just calls f), so x's
shapeCell stays at the sentinel".  STG_DEFAULT_ON §"What this does
NOT fix" confirms `v3-direct` failure separately.

**Verdict:** Architecturally honest — the doc describes this as a
legitimate STG-perspective cycle, not a workaround gap.  The
implication is that v3-direct on nixpkgs requires further laziness
work in `lower.cc` (the `recurseIntoAttrs` eager force in
all-packages.nix:140; see `CALLPACKAGE_BUG_2026-05-09.md`).

### G. THUNK_ALL has a 100x+ nixpkgs perf cliff
**Reference:** `CELL_UPDATE_EVERYWHERE_2026-05-12.md:186-191` —
`pkgs ? lib` timeout at 120s+ under THUNK_ALL; same baseline 0.5s
under TW.  No synthetic workload reproduces the cliff.  Blocked on
CPU profiler access.

**Verdict:** The architectural fix is gated behind a perf problem
that hasn't been root-caused, and the perf data presented here is
synthetic-only — the v3-fhook+STG mode that **does** ship doesn't
exercise the cliff.

### H. Lode README index lag (continuing)
**Reference:** REVIEW_2026-05-09 §4 reported 15/17 indexing lag.

`grep README.md`: of the 6 docs cited in this audit's scope,
`CLEANUP_AUDIT_2026-05-09.md` is indexed; `STG_DEFAULT_ON_2026-05-09`,
`STG_INVENTORY_2026-05-09`, `PUBLISH_RECOVERY_USE_AUDIT_2026-05-08`,
`CELL_UPDATE_EVERYWHERE_2026-05-12`, and the new
`REVIEW_2026-05-11/` directory are all **missing**.

---

## Risks

### R1. Cell-everywhere never being turned on (or being broken when it is)
The cell-everywhere mechanism's correctness has never been validated
in default builds because it's gated off.  If a regression surfaces
that exposes a Phase 1.5 cell-write bug (e.g., racing innermost
detection in nested THUNK_RETURN frames; cell aliasing after
OP_APPLY_OVERRIDES re-allocates Bindings), there's no quick
fallback — the registry it would have fallen back to is deleted.

### R2. No opt-back if THUNK_ALL regresses
The unified runtime flag was Phase 3.1's `NIX_V3_NO_PARTIAL_BINDINGS`
(now default-on as the registry being empty).  Phase 3.3 deleted
the registry outright.  `NIX_V3_KEEP_PARTIAL_BINDINGS` was
advertised as the escape but never wired (see §B above).  The only
escape today is `NIX_V3_NO_INHERIT_FROM_THUNK_ALL=1`, which reverts
to the eager-inherit-from mode — but **without** the registry
behind it, the original libsForQt5 cycle returns.  In other words,
the only way to test the pre-cleanup state is to revert the
deletion commits.

### R3. Comments / docstrings out of sync with code
`bytecode.hh:207-274` still describes `OP_ATTRS_LET_REC_INIT`,
`OP_ATTRS_REC_INIT_TAIL`, and `OP_ATTRS_UPDATE_TAIL` in terms of
their publish semantics.  Those semantics are gone.  Future
readers will misread the opcode purpose.  Several `vm.cc` comments
similarly reference `publishToNearestBlackThunkFrame` /
`publishToAllThunkFrames` as if they still exist (lines 649,
4567, 4608, 4686, 4809, 5317, 7686).

### R4. Architectural-honesty risk: a paint job over an unsolved problem
The deletion is real and welcome.  But the v3-direct nixpkgs
failure persists, the THUNK_ALL perf cliff persists, and the
cell-everywhere mechanism that was supposed to replace the
registry is **opt-in only**.  An outside reader of
`CELL_UPDATE_EVERYWHERE_2026-05-12.md` may infer that
cell-everywhere is solving real problems in production builds; the
code shows it is solving them in **bench mode**.  This is exactly
the "narrative decoupling from evidence" pattern flagged in
REVIEW_2026-05-09 §B.

### R5. Future bug class shifted, not eliminated
The audit's S5 was a pure architectural pivot — TW thunkifies every
from-expr, v3 now does too under THUNK_ALL.  But TW's perf is
amortised over decades of inline tuning.  v3 added 4 new env-var
opt-outs in Phase 4 to mitigate the cost, none of which actually
removes the thunk-per-from-expr overhead.  Future bug class:
"thunk-allocation bottleneck in inherit-from-heavy code on v3".

### R6. Dead-code inventory accumulating
After today: CFF_TAINTED (dead flag), `NIX_V3_NO_STG_WHNF`
(comment-only env var), `NIX_V3_KEEP_PARTIAL_BINDINGS` (advertised
but absent), the OP_ATTRS_*_TAIL bytecode dispatch arm divergences
(now near-identical to non-TAIL).  Each adds ~tens of LOC of
deletable dead code, none individually critical, but the
CLEANUP_AUDIT_2026-05-09 already flagged 96 unique `NIX_V3_*` flags
in source — the count keeps growing.

---

## Honest scorecard

| Claim made by the arc | Reality in code |
|---|---|
| "Registry retired" | Verified: 7 deletion commits (3.3a-g) totalling ~600 LOC. |
| "Cell-update-everywhere is the sole mechanism" | **Partial.** Mechanism exists; default OFF. |
| "Opt back via `NIX_V3_KEEP_PARTIAL_BINDINGS=1`" (commit `16d0adc81`) | **No.** Flag advertised, never wired. |
| "All 10 THUNK_ALL regression suites pass" (every Phase 3 commit) | Trustable — same suite each time. |
| "v3-direct nixpkgs eval works" | **No.** Per CELL_UPDATE_EVERYWHERE:154 still fails. Per STG_DEFAULT_ON:52, the failure is pre-existing eval-order, not a Phase 3 regression. |
| "v3-fhook nixpkgs hello.name works" | Verified per STG_DEFAULT_ON matrix — but this is the older STG default-on flip (#547), not the #558 cleanup. |
| "100x+ THUNK_ALL nixpkgs perf cliff" | Honestly documented in CELL_UPDATE_EVERYWHERE; unmeasured root cause; blocks Phase 3.2's default flip from being a real win on the hardest target. |

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
