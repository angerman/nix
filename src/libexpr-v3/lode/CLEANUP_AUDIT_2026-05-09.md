# v3 Codebase Cleanup Audit (2026-05-09)

Six parallel agents audited the v3 evaluator at `src/libexpr-v3/` for
cleanup potential across six domains:
1. Bytecode opcodes — `bytecode.hh`, `vm.cc` dispatch, `emit.cc`, `disasm.cc`.
2. Hook-mode-only code — what becomes dead after `NIX_V3_DIRECT_EVAL`
   migrations.
3. Env-var / flag inventory — ~155 flags catalogued.
4. Dead code & abandoned experiments — `NOT WIRED`, vestigial paths,
   zero-caller symbols.
5. Test infrastructure — 35 test files (32 shell + 3 C++).
6. `lode/` documentation hygiene — 33 docs.

**The conservative bias paid off.**  Across six domains the agents found
**very little safe-to-delete-today**.  The bulk of cleanup is correctly
*gated on milestones* — `#498` closure, INVERSION Phases 2/3/4, the v5
disk-cache schema bump.  That is the right discipline; we shouldn't
delete the publish-walk recovery while it's still load-bearing for
default-mode users, and we shouldn't reclaim opcode bytes between
schema bumps.  This memo catalogues what's deletable now, what's
deletable later, and (critically) **what to keep even though it looks
dead** — the lesson-preservation inventory.

Confidence: **C** verified, **L** likely, **S** speculative.

---

## Executive summary

| Cleanup tier | Items | When |
|---|---|---|
| **Zero-risk, do today** | 6 small items + 1 doc + 8 README additions + 5 archive moves | Now |
| **At v5 disk-cache schema bump** | 6 reserved opcodes | Next schema event |
| **After #498 root-causes** | STG-1..6 flag family, publish-walk + side-table-recovery, ~6 flags | Open-ended (#498 not yet closed) |
| **After INVERSION Phase 3** | 5 hook-mode `v3EvalEntry` machinery items + 2 transitional | When Phase 3 lands (1-2 sprints) |
| **After INVERSION Phase 4** | 4 bridge-plumbing primops + their support tables | Phase 4 (next 1-2 months) |
| **Keep forever (forward-FFI direction)** | 5 items mistaken for hook-mode | Permanent |

**Headline numbers:**
- LIVE production opcodes: **65 of 71** (6 reserved-no-emit).
- Production hook-mode items deletable today: **0** (all still fire under `NIX_USE_V3=1` without `NIX_V3_DIRECT_EVAL`).
- Tests deletable: **0**; archive: **2 + README**.
- Lode docs deletable: **0**; archive: **3** (BENCH-2026-05-04 trio).
- Env-var flags retire-today: **3-4** (depending on conservatism).
- Dead-code lines deletable today: **~110** (`primop_canon.hh` is 106 of those).
- USAGE.md doc patches needed: **6 missing flags + 1 polarity error + 1 default value drift**.

---

## 1. Bytecode opcodes (Agent 1)

**Verdict: 65 LIVE, 6 RESERVED-NOEMIT, 0 vestigial.**  Reserved bytes
(`OP_NOP`=0x00, `OP_POP`=0x14, `OP_SWAP`=0x15, `OP_NEGATE`=0x24,
`OP_BRANCH_TRUE`=0x46, `OP_POS`=0xA1) are deliberately kept for
disk-cache fingerprint stability (`serialize.cc:90-185`).  Dispatch
arms compress to single comment lines; the `default:` aborts loudly.

Auxiliary structures audited:
- `AttrSelectIC` (4-way PIC, `bytecode.hh:297-308`) — live, hot path
  (`vm.cc:4160-4274`).  Keep.
- `forceEmitSites` side table (`bytecode.hh:314-331`) — populated, used
  only by `V3_DBG_FORCE_SITE`.  ~16 B per emit site, low cost.  Keep.

**Recommendation:** hold all opcode deletions until the v5 disk-cache
schema bump.  Reclaiming 6 bytes is mechanical but pointless in
isolation since the bytes cost nothing while reserved.  When v5
lands, delete the 6 enum entries, 6 disasm cases, and the
`bytecode.hh:46-99` doc block (preserve a one-liner: "0x00–0xA1
holes intentional, see git history").

**Newly catalogued:** `OP_THUNK_SET_LOCAL_THROUGH_CELL` (0x85) from
STG-14b — live, dispatched, decoded.

---

## 2. Hook-mode deprecation scope (Agent 2)

**Verdict: 0 deletable today; 5 DEPRECATE-NOW; 6 TRANSITIONAL; 5 KEEP.**

The audit catalogued 15 hook-mode plumbing items (`primV3CallBridge1`,
`__v3_force_attr`/`_list_elem`, `Tag::Bridge` state, the OP_FORCE
Bridge handler, `tryBridgeAttrLookup`, `willReturnClosure`,
fallback-reason codes, `tryDispatchBridge1Direct`,
`tryDispatchTWLambdaInV3` (STG-14a), `tryDispatchFormalsLambdaBridge`,
Phase E lift gates, active-v3-VM refusals, lambda-reentry guard,
`Thunk::forces` field, top-level Attrs/List short-circuit).

**Critical clarification: 5 items survive the inversion intact** —
they are forward-direction Bridge plumbing (TW value held by v3 +
v3 forcing TW values + v3 dispatching TW lambdas).  Phase 2 of the
inversion plan **uses** these.  They are not hook-mode artefacts:
- `Tag::Bridge` runtime state
- OP_FORCE Bridge handler
- `forceValue` Bridge case
- `tryBridgeAttrLookup` / `tryBridgeAttrHas`
- `tryDispatchTWLambdaInV3` (STG-14a)

These should not be flagged as hook-mode in any future memo.

**Phase ordering for retirement:**
- After Phase 2: nothing retires (Phase 2 actively uses Bridge).
- After Phase 3 (`v3EvalEntry` retired): `willReturnClosure`,
  `evalReasonNames`, Phase E lift gates, active-v3-VM refusals,
  lambda-reentry guard, top-level Attrs/List short-circuit.
- After Phase 4 (every TW-side closure-callback primop migrated to
  `applyClosure`): `primV3CallBridge1`, `__v3_force_attr`/`_list_elem`,
  `tryDispatchBridge1Direct`, `tryDispatchFormalsLambdaBridge`, plus
  their support tables (`g_bridgeCallBridge1Calls` counter,
  `v3BridgeClosures` table, `v3FormalsLambdaBridges` map).

**Action now:** add `[[deprecated("hook-mode only; remove after
INVERSION Phase 3")]]` markers to the 5 DEPRECATE-NOW items so
future contributors don't accrete more callers.

---

## 3. Env-var / flag inventory (Agent 3)

**Verdict: ~155 flags catalogued; bulk is correctly gated.**

Distribution:
- LIVE-DEFAULT: ~10
- LIVE-OPTIN: ~12
- A/B-DECIDED (vestigial / candidate to retire): ~14
- BISECT-HARNESS (gated by an open issue): STG-1..6 family +
  `NIX_V3_NO_LIFT_*` (3 sub-lifts) + 4 others
- BUG-GATED: `LIFT_IRWPC` (eachSystem inf-recursion);
  `ON_DEMAND_ROOT` family + `PARSE_PRECOMPILE` (#455)
- DIAGNOSTIC-LIVE: ~70
- DIAGNOSTIC-VESTIGIAL: handful
- MISSING-FROM-DOCS: confirmed for 6 flags

**Retire-today candidates (low-risk):**
- `NIX_V3_PUBLISH_NON_REC_INIT` — pre-#496 bisect knob; #496 closed
  in `7709cc20e`.  Permits restoring known-broken behaviour.
- `NIX_V3_SKIP_THRESHOLD` + `NIX_V3_SKIP_FORCE_LINES` — Reason-6
  retired (`kSkipThresholdFunctions = SIZE_MAX`); env vars read but
  observably dead.
- `NIX_V3_EAGER_ARG_FORCE` — laziness goal settled; no remaining
  users.

**Documentation patches required for `USAGE.md`:**
- Add: `NIX_V3_STG`, `NIX_V3_STG_KEEP_HOOKS`, `NIX_V3_NO_TW_LAMBDA_INV3`,
  `NIX_V3_NO_EARLY_PUBLISH`, `V3_DBG_TW_LAMBDA_INV3`, `V3_DBG_RETRY`,
  `NIX_V3_DIRECT_EVAL`.
- Correct: `NIX_V3_CALL_FORMALS` polarity (code reads `NO_CALL_FORMALS`;
  doc inverted).
- Update: `NIX_V3_SELF_DOT_MAX_LEVEL` default from 0 to 4 per #528
  (`917b4fc5a`).

**STG-* family disposition:** keep all kill-switches until #498 closes.
Per `project_498_always_thunkify_regression.md`, **#498 is NOT
closed**.  When it closes, ~6 flags drop in one cleanup pass.

---

## 4. Failed experiments / dead code (Agent 4)

**Verdict: 4 confident deletions today (~110 lines).**

| # | Item | Lines | Evidence |
|---|---|---|---|
| 1 | **`include/v3/primop_canon.hh`** | 106 | Header reads "Status: declarations only. The shims are intentionally not wired into either registry yet"; `.cc` does not exist; **zero references** to `adaptFromTw`/`adaptToTw`/`CanonPrimOpFn` outside the header itself.  Biggest single dead block. |
| 2 | `NIX_V3_RETURN_CHAIN` / `NO_RETURN_CHAIN` / `OUTER_WITH` / `NEVER_RUN_OD` doc references | small | Zero `getenv` sites; only `OPTIMIZATION_PLAN.md:85,181,211` mentions remain.  Comments + docs only. |
| 3 | `NIX_V3_PUBLISH_NON_REC_INIT` re-enable knob + 3 dead `publishToNearestBlackThunkFrame(..., /*isRecInit=*/false)` call sites | ~20 | Permits restoring known-broken pre-#496 behaviour; off-side dead since `7709cc20e`.  Call sites at `vm.cc:3762, 3829, 4387` are no-ops without that env var. |
| 4 | `kEvalFallbackCap = 8` → 7 | trivial | reasons array sized 8; indices 7 never bumped per grep. |

**Items already deleted (verified):** `clearBridgeTables`,
`__v3_call_bridge_2`, `Value::isApp`/`isSlot`, `struct ThunkDescriptor`,
`OP_LOAD_SLOT_REF` Phase 3 dispatch + emit.  REVIEW_2026-05-04 and
06b were correct that PR4 cleanup landed.  Update
`OPTIMIZATION_PLAN.md:25-31` to remove the stale "NOT WIRED" note.

**KEEP for emergency / not yet retirable:**
- `partialBindingsRegistry` + `publishToNearestBlackThunkFrame`
  (`vm.cc:807-932`) — disabled under STG but load-bearing for
  default mode.  Delete with the STG default flip when #498 closes.
- 6 reserved opcodes (cross-ref Agent 1).
- `fiber.cc` + `bridge_yield.cc` (~360 LoC) — opt-in WC-18
  experiment, default off.  Recommend gating compilation behind a
  Meson option rather than deleting.
- `NIX_V3_BRIDGE_CLOSURE` legacy flag — emergency opt-in pair with
  `NIX_V3_NO_INVERT_EVAL`.
- `Thunk::forces` field — diagnostic; one read site
  (`V3_DBG_BLACKHOLE_TRACE`); cost is 4 B/thunk + 1 inc/force.

**Dispatch loop hygiene** is excellent.  `vm.cc:1050-5386` is
well-tended; default arm aborts loudly.  Disk-cache layer is
strict-version-only (no backward-compat readers).

---

## 5. Test infrastructure (Agent 5)

**Verdict: 0 deletions; 2 archive moves.**

35 test files (32 shell + 3 C++) — all correctness scripts KEEP.

**Archive candidates** (move to `src/libexpr-v3/test/attic/`):
- `wc38-bisect-harness.sh`
- `run-wc38-nixpkgs-probe.sh`
- `wc38-bisect-README.md`

WC-38 closed by `c95be6461` and `685262a6f` six commits ago.  The
probe's own header says "Once the fix lands, this script becomes a
regular regression test" — but `run-wc-laziness-tests.sh` already
covers WC-38 cases.  Move to `attic/` so the bisect template stays
available without polluting the active runner list.

**`safe-nix-eval.sh`** (introduced for STG-14b memory-limited bisect)
— **keep**.  Generic OOM-bounded wrapper, reusable beyond #498.

**KNOWN-FAIL/SKIP markers:** `b4d7f78ba` already swept the stale
assertions; remaining mentions are narrative comments load-bearing
for context.  Keep.

**No duplicates warrant deletion.**  Mild overlaps
(`run-cutover-tests.sh` is a strict subset of
`run-cutover-parity-tests.sh`; `run-v3-tests.sh` overlaps the lang
corpus) are kept because they exercise different surfaces.

---

## 6. `lode/` documentation hygiene (Agent 6)

**Verdict: 17 LIVE-FORWARD, 11 HISTORICAL-VALUABLE,
1 CONTRADICTED, 3 SUPERSEDED, 1 STALE.**

**Move to `lode/attic/`** (3 files):
- `BENCH-2026-05-04-CUMULATIVE.md`
- `BENCH-2026-05-04-PHASE5-DEFAULT.md`
- `BENCH-REAL-WORLD-2026-05-04.md`

All three already carry "Superseded" banners; `BENCH-2026-05-08-POST-530.md`
is now canonical.

**README missing entries — add 8** (existing 2026-05-08 audits/plans
not yet indexed):
- `CELL_UPDATE_AUDIT_2026-05-08.md`
- `EVAL_ORDER_DIVERGENCE_2026-05-08.md`
- `SLOT_AUDIT_HOOK_REENTRY_2026-05-08.md`
- `PUBLISH_RECOVERY_USE_AUDIT_2026-05-08.md`
- `SLOT_TAGGING_AUDIT_2026-05-07.md`
- `LEXICAL_WITHS_PLAN_2026-05-08.md`
- (plus the docs already added: `OPT_OCCUR_PLAN_2026-05-08.md`,
  `VM_TW_MEASUREMENT_2026-05-09.md`)

**Add `STATUS:` headers to 4 docs:**
- `TEAM_A_B.md` — claims `lower.cc` is 2227 LOC; actual **2642**;
  the 2-week prep sprint never ran; INVERSION pivot reshapes the
  team-split boundary.  Tag STALE.
- `FFI_PLAN_2026-05-06.md` — pre-INVERSION; point forward at
  `INVERSION_PLAN_2026-05-08.md`.
- `REVIEW_2026-05-04.md` and `REVIEW_2026-05-06.md` — the bridge-
  closure thread evolved through `INVERSION_PLAN`; add forward-link.

**Don't delete the contradicted docs.**  `TRAFFIC-OWNERSHIP-REVIEW.md`
(its "call-hook never wired" thesis was wrong at write time) is
preserved as the cautionary case study.  `WC38_FIX_PLAN.md` is the
"predicted vs landed" cautionary tale.  Keep.

---

## Cross-cutting themes

### A. Most cleanup is correctly milestone-gated

The audit found very little safe-to-delete-today.  This is the right
discipline.  Cleanup gated on:

- **#498 closing** — STG-1..6 flag family, publish-walk machinery,
  partialBindings registry, several bisect knobs.
- **INVERSION Phase 3** (other CLI commands gated through v3-direct)
  — ~5 hook-mode `v3EvalEntry` items.
- **INVERSION Phase 4** (every TW-side closure-callback primop
  through `applyClosure`) — 4 bridge-plumbing primops + tables.
- **v5 disk-cache schema bump** — 6 reserved opcodes.

### B. The "forward Bridge direction survives" clarification

The Bridge runtime machinery (`Tag::Bridge`, OP_FORCE handler,
`tryBridgeAttrLookup`, `tryDispatchTWLambdaInV3`) is **not** hook-mode
plumbing.  It supports the v3 → TW *forward* call direction (v3
holding a TW value and forcing it on demand), which Phase 2 of the
inversion actively requires.  Earlier audits sometimes lumped this
in with retirable hook-mode plumbing; future memos should not.

### C. Documentation accumulates faster than indexing

8 audit/plan docs landed 2026-05-08 without README entries.  This is
small but recurrent — every sprint adds 1-3 docs.  Recommend:
require a one-line `lode/README.md` entry as part of any new doc PR.

### D. The largest single dead block is `primop_canon.hh`

106 lines of header-only declarations explicitly marked "intentionally
not wired."  Verified zero references in tree.  Single-PR cleanup.

### E. Conservative bias should hold

Several items look dead but are actually emergency levers:
`NIX_V3_BRIDGE_CLOSURE`, `NIX_V3_NO_INVERT_EVAL`, the legacy chain-push
already deleted but its env-var stubs.  Keep emergency levers until
the corresponding architectural piece is universally proven.

---

## Action items, prioritised

### Tier 0 — do today (zero risk, ~150 LOC + doc)

1. **Delete `include/v3/primop_canon.hh`** (106 lines).  Verify no
   external consumers via the build system; if any, mark
   `[[deprecated]]` first.
2. **Delete `NIX_V3_PUBLISH_NON_REC_INIT` env knob** + the 3 dead
   `publishToNearestBlackThunkFrame(..., /*isRecInit=*/false)` call
   sites at `vm.cc:3762, 3829, 4387`.
3. **Delete `NIX_V3_SKIP_THRESHOLD` + `NIX_V3_SKIP_FORCE_LINES`**
   env knobs (Reason-6 retired).
4. **Delete `NIX_V3_EAGER_ARG_FORCE`** env knob (decision settled).
5. **Reduce `kEvalFallbackCap`** from 8 to 7.
6. **Update `OPTIMIZATION_PLAN.md:25-31`** to remove the stale
   "NOT WIRED" note about Phase 3 OP_LOAD_SLOT_REF (the dispatch
   was already deleted by PR4 cleanup).

### Tier 0' — doc / index hygiene (half a day)

7. **Patch `USAGE.md`** for the 6 missing flags + 1 polarity
   correction (`NIX_V3_CALL_FORMALS`) + 1 default-value drift
   (`NIX_V3_SELF_DOT_MAX_LEVEL`).
8. **Index 8 missing docs** in `lode/README.md`.
9. **Add `STATUS:` headers** to `TEAM_A_B.md` (STALE),
   `FFI_PLAN_2026-05-06.md`, `REVIEW_2026-05-04.md`,
   `REVIEW_2026-05-06.md`.
10. **Create `lode/attic/`** + move the 3 BENCH-2026-05-04 files.
11. **Create `src/libexpr-v3/test/attic/`** + move
    `wc38-bisect-harness.sh`, `run-wc38-nixpkgs-probe.sh`,
    `wc38-bisect-README.md`.
12. **Mark 5 hook-mode items `[[deprecated]]`**: `willReturnClosure`,
    `tryDispatchFormalsLambdaBridge`, Phase E lift gates, top-level
    Attrs/List short-circuit, the 6 fallback-reason codes.

### Tier 1 — at v5 disk-cache schema bump

13. Reclaim 6 reserved opcode bytes (0x00, 0x14, 0x15, 0x24, 0x46,
    0xA1).  Strip enum entries, disasm cases, dispatch comments,
    long doc block in `bytecode.hh:46-99`.

### Tier 2 — when #498 root-causes (open-ended)

14. Retire `NIX_V3_STG`, `NIX_V3_STG_KEEP_HOOKS`, the 6-step kill
    switch family.  Delete `partialBindingsRegistry`,
    `publishToNearestBlackThunkFrame`, the side-table-recovery code
    in `OP_REC_BINDING_SLOT_REF`.  ~700-1000 LOC.

### Tier 3 — after INVERSION Phase 3

15. Delete `v3EvalEntry` and downstream: `willReturnClosure`,
    `evalReasonNames`, `liftLambda`/`liftAttrsList`/
    `liftWillReturnClosure`/`liftIRWillProduceClosure` gates,
    active-v3-VM refusals, lambda-reentry guard, top-level
    Attrs/List short-circuit.

### Tier 4 — after INVERSION Phase 4

16. Retire `primV3CallBridge1`, `__v3_force_attr`/`_list_elem`,
    `tryDispatchBridge1Direct`, `tryDispatchFormalsLambdaBridge`.
    Delete `g_bridgeCallBridge1Calls`, `v3BridgeClosures`,
    `v3FormalsLambdaBridges`.

### Tier 5 — never (preserve)

17. Reserved opcodes' enum values (require disk-cache fingerprint
    stability).
18. Forward-direction Bridge plumbing (`Tag::Bridge`, OP_FORCE
    Bridge handler, `forceValue` Bridge case, `tryBridgeAttrLookup`,
    `tryDispatchTWLambdaInV3`).
19. `Thunk::forces` field (diagnostic; tiny cost).
20. `fiber.cc` / `bridge_yield.cc` — recommend Meson-gated
    compilation rather than deletion (~360 LoC saved from default
    builds).
21. Contradicted/historical lode docs — `TRAFFIC-OWNERSHIP-REVIEW.md`,
    `WC38_FIX_PLAN.md`, OPTIMIZATION_PLAN's chronological log.
    Lessons matter; preserve.

---

## Verdict

**The codebase is in good cleanup discipline.**  Across 35 test files,
~155 env vars, 71 opcodes, 33 lode docs, and the C++ source, the
agents found:

- ~110 LoC deletable today (mostly `primop_canon.hh`).
- 0 production hook-mode items deletable today.
- 0 tests deletable.
- 0 lode docs deletable.
- 5 archive moves (3 docs + 2 test scripts).
- 1 STALE doc + 4 status-header retrofits.
- 6 USAGE.md doc patches.

The bulk of cleanup is **correctly gated** on the natural milestones
the project is already pursuing: closing #498, INVERSION Phases 2/3/4,
the next disk-cache schema bump.  Premature deletion would either
break default-mode users (#498 territory) or thrash the disk cache
(opcode renumbering).  The conservative discipline that produced this
verdict is the right discipline to maintain.

The single highest-leverage cleanup PR available **today** is the
combined Tier 0 + Tier 0' set: ~150 LoC source deletion + the 6 doc
patches + the README + STATUS retrofits + the two attic moves.
Combined diff is < 300 lines; reviewer effort ~half a day; outcome
is a substantially tidier tree without touching any milestone-gated
machinery.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
