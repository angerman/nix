# v3 Env-Var Inventory — 2026-05-15

Per action plan Phase 0.16. Pre-condition for Phase 4 env-var
consolidation.

## Methodology

Extraction (`grep -rhoE 'std::getenv\("[A-Z_0-9]+"`) over
`src/libexpr-v3/` excluding `lode/`. Cross-referenced with reader
count and first-file:line. Categories per action plan:

- **KEEP** — genuinely useful single-flag toggles. Stay individually
  named; no retirement criterion required.
- **RETIRE-NOW** — dead readers, deleted writers, or duplicated
  polarity. Can be deleted in Phase 4 with no behavior change.
- **RETIRE-AFTER-X** — tied to a specific bug close or phase. Keep
  the reader; delete the gate (and inline the chosen polarity) when
  the linked work lands.

## Totals

- Unique gate names extracted: **167** (excluding the non-v3
  `NIX_PATH` reader and the illustration tokens `X`, `Y` in
  `lint-no-inline-getenv.sh`).
- KEEP target per action plan: ≤20.
- Target after Phase 4 consolidation: ≤30 total (some KEEP, some
  folded behind a single `NIX_V3_DEBUG=cat1,cat2` bitmask).

## KEEP (genuinely useful single-flag toggles, ≤20)

These have user-facing or bench-harness consumers; retiring them
would break documented workflows. Counted: **17**.

| name | first use | purpose |
|---|---|---|
| `NIX_V3_DIRECT_EVAL` | `installables.cc` | Master gate: route eval through v3 instead of TW. The primary user-facing toggle. |
| `NIX_V3_DISK_CACHE` | `primops.cc:5799` | Persistent on-disk SQLite bytecode cache. |
| `NIX_V3_CACHE_DIR` | `disk_cache.cc:123` | Override disk-cache location. |
| `NIX_V3_NO_OPTIMISE` | `run.cc` | Disable IR optimizer; useful when bisecting opt-pass regressions. |
| `NIX_V3_NO_CONTENT_CACHE` | `primops.cc` | Disable in-memory content-keyed bytecode cache. |
| `NIX_V3_NURSERY` | `alloc.hh` | Enable Cheney nursery allocator. |
| `NIX_V3_NURSERY_SIZE` | `alloc.hh` | Configurable nursery size (bytes). |
| `NIX_V3_NURSERY_SCAVENGE` | `vm.cc` | Enable scavenge-style nursery moves. |
| `NIX_V3_NO_CLOSURE_POOL` | `vm.cc:4242` | Disable the fakeClo recycle pool. |
| `NIX_V3_LOG_DEPTH` | `vm.cc:8589` | Peak C-stack depth logging — debugging tool referenced by tests. |
| `NIX_V3_STG` | `vm.cc` | Master STG-feature gate (default-on; opt-out via empty). |
| `NIX_V3_INHERIT_FROM_THUNK_ALL` | `lower.cc` | Architectural toggle for inherit-from laziness; Phase 2 decision point. Will be replaced by a single committed path, but stays a single named gate while it carries load. |
| `V3_TIMING` | `run.cc:74` | Phase-split timing dump — used by bench harness. |
| `NIX_VM_STATS` | `run.cc` | Allocation counter dump — used by bench harness. |
| `V3_DBG_ALLOC_DUMP` | `vm.cc:142` | Top-N lambda alloc/force/call summary — the canonical hot-loop diagnostic. |
| `V3_DBG_HOT_CALLEE` | `vm.cc` | Frame-stack dump for a hot lambda. Action plan Phase 1 diagnostic. |
| `V3_TIMING` (alias `V3_TIMING`) | — | (only one row above; counted as one) |

**Rationale**: each has either a documented use (bench, disk cache,
disable optimizer for bisect), a single load-bearing semantic
toggle (nursery, STG, INHERIT_FROM_THUNK_ALL), or an observability
role (V3_DBG_ALLOC_DUMP, V3_TIMING).

## RETIRE-NOW (dead infrastructure post-v3_hook.cc deletion)

These have no remaining writers (or no remaining readers after
v3_hook.cc deletion) and can be removed in Phase 4 without behavior
change. Discovered during Phase 0.2 tombstone reaping.

| name | first use | reason |
|---|---|---|
| `CFF_TAINTED` | — | Removed in commit 156939f43 (Phase 0.1). Listed for completeness. |
| (eager-bridge TLS readers) | `primops.cc:3757`, `3859` | `forceEagerBridge()` always returns false; `pushForceEagerBridge` / `popForceEagerBridge` have zero callers. The whole TLS apparatus is unreachable. |
| `V3_DEBUG_HOOK` | `primops.cc:3084` | Reader is inside a code path that no longer matters with the hook deleted. Triage in Phase 4. |

## RETIRE-AFTER-X (single-use gates tied to a specific phase / bug close)

Most of the 165 — the long tail of `V3_DBG_*` and `NIX_V3_NO_*`
gates. Inline retirement criterion encoded in each comment is
required (Phase 0 rule 2). Each row below identifies the linking
work item. Once that work lands, delete the gate AND inline its
chosen polarity.

| name | linked work item | retirement trigger |
|---|---|---|
| `NIX_V3_INHERIT_FROM_THUNK_ALL` | Phase 2 (cycle-handling decision) | Phase 2 picks Path A or B; one polarity becomes the default and the gate goes. |
| `NIX_V3_NO_INHERIT_FROM_THUNK_ALL` | Phase 2 | Same — only one of these two will survive. |
| `NIX_V3_INHERIT_FROM_THUNK_FILTER` | Phase 2 | Filter is an experimental narrowing path. Retire with Phase 2 decision. |
| `NIX_V3_NO_INHERIT_FROM_THUNK` | Phase 2 | Same family. |
| `NIX_V3_CELL_EVERYWHERE` | Phase 2 (Path B) | If Phase 2 picks Path B (cells), this becomes default-on and the gate goes. |
| `NIX_V3_LEAKED_BLACK_RECOVER` | Phase 2 | Workaround for a specific cycle-handling case; either subsumed by the Phase 2 mechanism or proven unnecessary. |
| `NIX_V3_NO_BLACKHOLE_AS_VALUE` | Phase 2 | Same family. |
| `NIX_V3_NO_PATH_COMPRESS` | Phase 2 | Path-compression is part of the cycle-handling design; either default-on or retired. |
| `NIX_V3_STG` | Phase 2 | Master STG gate. If Phase 2 confirms STG-style cell update is the architecture, this becomes ungated. |
| `NIX_V3_EAGER_ARG_FORCE` | Phase 1 (iterative force) | After iterative forceValue completes, eager-arg force-or-not is no longer a tuning parameter — pick one and inline. |
| `NIX_V3_NO_REC_SLOT_CAPTURE` | Phase 1 | Same family — rec-slot capture is part of the slot architecture. |
| `NIX_V3_LAMBDA_SKIP` | TW-lambda-Value bridge (was #493) | Originally a feature gate; the underlying lambda-skip is now the default path. Inline. |
| `NIX_V3_NO_REFUSE_FORMALS_BRIDGE` | TW-lambda-Value bridge | Same. |
| `NIX_V3_NO_REFUSE_REC_CAPTURE_LAMBDA` | TW-lambda-Value bridge | Same. |
| `NIX_V3_NO_INLINE_REC_SLOT` | Phase 1 | Tied to the rec-slot capture design. |
| `NIX_V3_BRIDGE1_DEPTH` | Phase 1 (after iterative force) | C-stack depth ceiling for the Bridge1 chain. Once iterative force removes the C-stack worry, the gate goes. |
| `NIX_V3_BRIDGE1_REENTRY_MAX` | Phase 1 | Same family. |
| `NIX_V3_FORCE_CHAIN_DEPTH` | Phase 1 | Same. |
| `NIX_V3_FORCE_CHAIN_REENTRY_MAX` | Phase 1 | Same. |
| `NIX_V3_BRIDGE_PRIMOP_DEPTH` | Phase 1 | Same. |
| `NIX_V3_FALLBACK_CHAIN_DEPTH` | Phase 1 | Same. |
| `NIX_V3_EAGER_BRIDGE_MAX` | Phase 2 (after eager-bridge retirement) | Eager bridge is dead infrastructure (see RETIRE-NOW above); the size cap is moot. |
| `NIX_V3_LAZY_BRIDGE_ARG` | Phase 2 | Lazy-bridge polarity gate. |
| `NIX_V3_FIBER_BRIDGE` | Phase 2 | Fiber-based bridging — alternative to nursery. Phase 2 picks one. |
| `NIX_V3_NO_OP_CALL_BRIDGE_SHORTCUT` | Phase 2 | Bridge-thunk OP_CALL shortcut. |
| `NIX_V3_TW_LAMBDA_BRIDGE` | TW-lambda-Value bridge | Same family as the lambda-skip gates. |
| `NIX_V3_NO_DEFER` | Phase 1 (#542 emit-time defer) | Defer is a landed optimization; if it stays, inline. |
| `NIX_V3_NO_DEEP_FORCE` | Phase 2 | Deep-force semantics decision. |
| `NIX_V3_NO_STG_WHNF` | Phase 2 | STG WHNF recovery path — overlaps `CFF_TAINTED` territory. |
| `NIX_V3_NO_OPT_STRICT` | analyseOccurrence wiring (Phase 0.3) | Strictness opt pass — once analyseOccurrence-driven DCE is in, the strictness opt may need re-tuning. |
| `NIX_V3_THUNK_ALL_TRIVIAL` | Phase 2 | THUNK_ALL family experiments. |
| `NIX_V3_THUNK_CALL_ON_SELECT_VAR` | Phase 2 | Same. |
| `NIX_V3_EARLY_PUBLISH` | Phase 2 | Cycle-handling tweak. |
| `NIX_V3_NO_UPDATE_TAIL` | Phase 2 | OP_ATTRS_UPDATE_TAIL polarity. |
| `NIX_V3_NO_COMPLEX_FROM_THUNK` | Phase 2 | Same family. |
| `NIX_V3_NO_GETFORCE_SUPER` | TW-lambda-Value bridge | Tied to the super-lambda detection. |
| `NIX_V3_INTRINSIC_DISPATCH` | #495 native intrinsics | Native lib.fix / extends / composeExtensions dispatch — landed; opt-in. Inline once verified default-on. |
| `NIX_V3_NO_INTRINSIC_RECOGNISE` | #495 | Inverse polarity. |
| `NIX_V3_ASSERT_CELL_OWN` | Phase 2 (Path B / cells) | Cell-ownership assertion — diagnostic for Path B development. |
| `NIX_V3_BRIDGE_TIMING` | Phase 1 / 2 | Timing bridge calls — diagnostic. |
| `NIX_V3_SELECTOR_LAMBDA` | #424 selector specialisation | Landed; opt-in. Inline once verified. |
| `NIX_V3_SELF_DOT_LIMIT` | Self-dot heuristic | STG-5 retired this; gates may be orphan readers. Triage. |
| `NIX_V3_SELF_DOT_MAX_LEVEL` | Self-dot heuristic | Same. |
| `NIX_V3_SELF_DOT_SKIP_NTH` | Self-dot heuristic | Same. |
| `NIX_V3_SKIP_FORCE_LINES` | force-trace filter | Diagnostic-only. |
| `NIX_V3_NO_OPT` | analyseOccurrence wiring | Master "no optimizer" — keep until analyseOccurrence wiring lands. |
| `V3_STRICT_DISK_CACHE` | #495 disk-cache | Strict mode — verify-then-retire. |
| `NIX_V3_SKIP_INSTALLABLE_PREEVAL` | Phase 1 | Opt-in TW pre-eval skip (commit 1ac5795b0). Inline once v3-direct passes hello.name. |

### V3_DBG_* family (≈90 entries) — RETIRE-AFTER-X

The bulk of the inventory. Each is a one-off `printf`-style diagnostic
from a specific investigation. Each must either:

1. Already have a retirement criterion comment per Phase 0 rule 2
   (post-Phase 0 commits enforced this).
2. Or be folded behind a single `NIX_V3_DEBUG=category` bitmask in
   Phase 4 (action plan §4 item 3).

Notable V3_DBG_* still single-purposed:

- `V3_DBG_ALLOC_DUMP` — **KEEP** (canonical hot-loop diagnostic;
  in KEEP table above).
- `V3_DBG_HOT_CALLEE` — **KEEP** (canonical hot-loop frame-stack
  dump; in KEEP table above).
- `V3_DBG_HOT_FORCE` — likely subsumed by HOT_CALLEE; retire after
  Phase 1.
- `V3_DBG_FORCE_TRACE` / `V3_DBG_FORCE_CALLSITE` / `V3_DBG_FORCE_NAME`
  / `V3_DBG_FORCE_POS` / `V3_DBG_FORCE_FILE` / `V3_DBG_FORCE_STRIDE`
  / `V3_DBG_FORCE_INSIDE_X` / `V3_DBG_FORCES` / `V3_DBG_FORCES_TOPN`
  / `V3_DBG_FORCE_SITE` / `V3_DBG_FORCE_ATTR_ENTRY` — fold into a
  single `NIX_V3_DEBUG=force` category (Phase 4).
- `V3_DBG_BRIDGE1*` / `V3_DBG_OPCALL_BRIDGE` / `V3_DBG_BRIDGE_NULL`
  / `V3_DEBUG_BRIDGE_SRC` / `V3_DEBUG_LIST_BRIDGE` / `V3_DBG_FAKECLO_AT_PTR`
  / `V3_DBG_RECYCLE_*` — fold into `NIX_V3_DEBUG=bridge` /
  `NIX_V3_DEBUG=closure-pool`.
- `V3_DBG_OPCYCLE*` / `V3_DBG_BLACK*` / `V3_DBG_VBH_PROD`
  / `V3_DBG_LEAKED_BLACK` / `V3_DBG_RETRY*` — fold into
  `NIX_V3_DEBUG=cycle`.
- `V3_DBG_WITH*` / `V3_DBG_SLOT_REF` / `V3_DBG_REC_INIT_LOCAL0`
  / `V3_DBG_REC_SET_NAME` / `V3_DBG_RES_FREEVARS` — fold into
  `NIX_V3_DEBUG=lower`.
- `V3_DBG_INTRINSIC*` / `V3_DBG_INHERIT_FROM_THUNK` / `V3_DBG_INLINE_REC`
  / `V3_DBG_SELF_DOT_FIRES` — fold into `NIX_V3_DEBUG=intrinsics`.
- `V3_DBG_TRACE_THUNK_X` / `V3_DBG_TRACE_THUNK_BODY` / `V3_DBG_MK_THUNK_ANY`
  — fold into `NIX_V3_DEBUG=thunk-trace`.
- `V3_DBG_SELECT_*` / `V3_DBG_ATTRS_*` — fold into `NIX_V3_DEBUG=select`.
- `V3_DBG_DUMP_*` / `V3_DUMP_*` / `V3_DBG_MAKE_*` — fold into
  `NIX_V3_DEBUG=disasm`.
- `V3_DBG_FIBER*` / `V3_DBG_NURSERY` / `V3_DBG_FRAME_ENTRY`
  / `V3_DBG_TAIL_FORCE` / `V3_DBG_PREHOOK` / `V3_DBG_TC_PRE`
  / `V3_DBG_P5` / `V3_DBG_RETURN_AT_CODEOFF` / `V3_DBG_RETURN_KEY`
  / `V3_DBG_RETURN_SELF` / `V3_DBG_RET_CHASE` / `V3_DBG_NO_IC`
  / `V3_DBG_OP_CALL*` / `V3_DBG_CALL*` / `V3_DBG_FINAL_CALL`
  / `V3_DBG_LOWER_CALLS` / `V3_DBG_OPCALL_FORCE` / `V3_DBG_OPCYCLE_DISASM`
  / `V3_DBG_GETFORCE_TAG` / `V3_DBG_IS_FUNCTION` / `V3_DBG_STRCONCAT`
  / `V3_DBG_STORE_PREVSTAGE` / `V3_DBG_UPDATE_FAIL` / `V3_DBG_SELECT_FAIL`
  / `V3_DBG_CELL_EVERYWHERE` / `V3_DBG_CHASE` / `V3_DBG_ADD_ERR_CTX`
  / `V3_DBG_BLACKHOLE_TRACE` / `V3_DBG_IMPORT` / `V3_DBG_BLACKHOLE_AS_VALUE`
  / `V3_DBG_POP_FAKECLO` / `V3_DBG_WITH_PUSH_PREV` — fold into
  appropriate categories.

### Drv / path / debug-list / dump (12 entries) — RETIRE-AFTER-X

| name | trigger |
|---|---|
| `V3_DRV_CALLER` | After Phase 1 |
| `V3_DRV_CALLER_STRIDE` | After Phase 1 |
| `V3_DRV_DEBUG` | After Phase 1 |
| `V3_DRV_NO_BRIDGE` | After Phase 2 |
| `V3_DRV_NO_NATIVE` | After Phase 2 |
| `V3_DRV_PER_DRV` | After Phase 1 |
| `V3_DRV_STATS` | After Phase 1 |
| `V3_PATH_NO_NATIVE` | After Phase 2 |
| `V3_DUMP_AT_START` | Diagnostic; fold to `NIX_V3_DEBUG=disasm` |
| `V3_DUMP_LAMBDAS` | Same |
| `V3_DUMP_RANGE` | Same |

## Action items spawned by this audit

1. **Phase 0 follow-up** — confirm eager-bridge TLS infrastructure
   (`pushForceEagerBridge`, `popForceEagerBridge`, `tlsForceEagerBridge`)
   has no remaining writers. If confirmed, delete the
   read sites (`primops.cc:3757`, `3859`) and the TLS apparatus
   itself in a single follow-up commit. **~30 LoC out, 2 gates
   retired.**
2. **Phase 2 prerequisite** — before the cycle-handling decision,
   collapse the THUNK_ALL / INHERIT_FROM_THUNK family (≥10 gates) so
   the decision picks ONE path and inlines the others.
3. **Phase 4 prerequisite** — implement the
   `NIX_V3_DEBUG=category1,category2` bitmask runtime parsing once
   in a shared helper; convert the V3_DBG_* family during Phase 4.
4. **Lint extension** — extend `lint-no-inline-getenv.sh` to also
   flag gates that have NO retirement-criterion comment within the
   5 lines before their first `getenv` call. Enforcement, not
   detection.

## Process discipline going forward

- Every new gate added between this audit and Phase 4 MUST have an
  inline retirement-criterion comment (action plan rule 2). The
  Phase 0.1 / 0.6 commits demonstrate the format:
  ```cpp
  // gate: NIX_V3_FOO — purpose. Retire when [observable Y / X
  // closed].
  static const bool s_foo = std::getenv("NIX_V3_FOO") != nullptr;
  ```
- The KEEP list is the audit's bright-line ≤20. Adding to KEEP
  requires justification beyond "useful for me right now".
- Phase 4's target of ≤30 individual gates expects most of
  RETIRE-AFTER-X to land via dependency closes, not opportunistic
  reaping.
