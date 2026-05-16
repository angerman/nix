# v3 Reduction Audit — Path to Native STG/V8 VM (2026-05-09)

Six parallel agents analysed the v3 evaluator at `src/libexpr-v3/`
for **what should not exist** in the target architecture: a v3-native
STG/V8-inspired bytecode VM with FFI only for leaves that can't
reasonably live in the VM (store I/O, fetchers, parser,
file-system, derivationStrict hash phase, `builtins.path` NAR work).

This is the *opinionated reduction* counterpart to
`CLEANUP_AUDIT_2026-05-09.md`.  The cleanup audit asked "what's
dead?" and answered conservatively (~110 LoC deletable today, bulk
milestone-gated).  This audit asks "what shouldn't exist in the
target?" and answers aggressively: **roughly 10,000 LoC of
hook-mode + return-bridge + speculative-FFI + bug-class-workaround
infrastructure is on the wrong side of the architectural line.**

The conservative-vs-opinionated tension resolves correctly: the
cleanup audit governs *deletions today*; this audit governs
*deprecation markers, milestone planning, and what counts as
"on-architecture" code review*.  Both apply.

Confidence tags **C/L/S**.

---

## Headline

| Domain | LOC to drop | Where |
|---|---:|---|
| 1. Hook-mode infrastructure | **~4,170** | `v3_hook.cc` body almost entirely; tail dies with Phase 3 |
| 2. TW-side bridge primops + return-direction | **~2,800** | `primops.cc` bridge primops + `fiber.cc` + `bridge_yield.cc` |
| 3. FFI categories & speculative ABI | **~340** | `ffi.hh` 11 of 13 categories, `Plugin ABI Option B`, and `~50 LoC` of `ffi.cc` Cat-C bodies |
| 4. STG bisect harness + identity-bug workarounds | **~640** | `vm.cc` publish-walk + recovery, `lower.cc` heuristic stack, 11 env-var flags |
| 5. IR / lower / emit reductions | **~510** lower.cc + ~12 fields | `lower.cc` heuristic stack (overlaps Domain 4), IR field drops, 6 reserved opcodes |
| 6. Tests + lode/ docs | **~10 of 27** tests retire; **~8 docs** to attic now | `test/` and `lode/` |

**Total source-side reduction: ~8,500 LoC committed code**, plus
deletions in support files.  After all milestones land,
`v3_hook.cc` is **deleted entirely**, `lower.cc` shrinks from 2728 to
~1500 LoC (with structural splits), and `ffi.hh` shrinks from 532 to
~120 LoC.

**Critical sequencing:** the keystone is **LexicalFromRef** (per
REVIEW_2026-05-09 §3) — generalising #530's static lexical-with
chain to the entire inherit-from / let-rec from-expr surface.  This
enables ~640 LoC of cleanup in Domain 4 (heuristic stack + workaround
machinery) and unlocks the structural fix that makes the bug class
deletable rather than managed.

---

## 1. Hook-mode infrastructure (Agent 1)

The eval-hook architecture is **one cut**, not many.  Every gate,
refusal, depth-limit, blacklist, and re-entry guard in
`v3CallFunctionEntry` is a "TW called us; can we honour the dispatch?"
question.  Under v3-as-host, v3 *issued* the call; the question is
moot.

**Drop list:**

| Tier | Items | LoC |
|---|---|---:|
| **DROP NOW** (already bypassed by v3-direct) | `v3EvalEntry` body (~950); fallback-reason machinery (~200); Phase E lift gates (~70); `willReturnClosure` static walk (~35); top-level Attrs/List/trivial-kind short-circuits (~25); closure-result refusal at hook exit (~165); `v3HookCache` + `v3HookContentCache` (~55); active-v3-VM refusals (~25); `installEvalHook` glue (~40) | **~1,565** |
| **DROP AFTER PHASE 3** (legacy CLI not yet on `NIX_V3_DIRECT_EVAL`) | `v3CallFunctionEntry` body (~840); `v3RegisterExprHook` + `v3RegisterExprEntry` (~70); `v3SubExprCache` + `populateSubExprCacheLocal` + `UpvalueSource` + variants (~520); `prepHookUpvaluesAndWiths` + `HookPrepResult` (~290); on-demand-root + `NIX_V3_ON_DEMAND_ROOT*` (~280); `NIX_V3_PARSE_PRECOMPILE` (~25); `tryDispatchBridge1Direct` (~120); `tryDispatchFormalsLambdaBridge` (~100); `s_refuseRecCapture`, `s_refuseWrongShape` (~30); call-hook telemetry (~80); `lowerCompileAndPopulate` glue (~125) | **~2,480** |
| **DROP AFTER PHASE 4** (depends on `applyClosure` migration) | `ScopedBridgeFallbackExpr` + `fallbackExpr` plumbing (~150) | **~150** |

**End state:** `v3_hook.cc` shrinks from 4,147 LoC → **~150 LoC**
(only `tryDispatchTWLambdaInV3`, the STG-14a forward-direction
shortcut), then to **0 LoC** (rename / fold into `primops.cc`)
after Phase 4.

**The key clarification:** several items look hook-mode but are
**forward-direction Bridge plumbing** — these survive untouched:

| Item | Why it survives |
|---|---|
| `Tag::Bridge` runtime state | v3 holds TW values from store/IO leaves, forces on demand |
| OP_FORCE Bridge handler | Same |
| `forceValue` Bridge case | Same |
| `tryBridgeAttrLookup` / `tryBridgeAttrHas` | v3 reading TW attrset on demand (e.g., flake-resolved set) |
| `tryDispatchTWLambdaInV3` (STG-14a) | Forward direction: v3 dispatches a TW-resolved lambda's body |
| `treeWalkerToV3` non-closure shapes | Phase 2 one-shot bridge for flake installables |
| `setNixEvalState` | Thread-local that primops use to reach TW for store/IO |
| `ScopedActiveV3VM` / STG-10 single-VM-per-thread | Architectural invariant |

---

## 2. TW-side bridge primops + return-direction (Agent 2)

The **TW-host-calls-into-v3** direction is one architectural commitment.
"v3 returns a Tag::Closure; TW wraps as PrimOpApp; TW redispatches via
`__v3_call_bridge_1`" is the entire design.  Under target architecture,
TW invokes v3 closures via `applyClosure` only — no PrimOpApp wrapper,
no shadow handle table, no fiber-bounded stacks.

**Drop list:**

- `primV3CallBridge1` + registration (~455 LoC)
- `primV3ForceAttr` / `primV3ForceListElem` + registrations (~370 LoC)
- `v3BridgeClosures()` / `v3BridgeAttrs()` / `v3BridgeLists()` /
  `v3FormalsLambdaBridges()` tables (~140 LoC)
- `tryUnwrapBridge1Closure` (~25 LoC)
- `bridge1DepthCounter` family + `BRIDGE1_REENTRY_MAX` (~70 LoC)
- `BridgePrimopDepthGuard` + `NIX_V3_BRIDGE_PRIMOP_DEPTH` (~40 LoC)
- `ForceChainGuard` family + `NIX_V3_FORCE_CHAIN_DEPTH` (~115 LoC)
- `ScopedBridgeFallbackExpr` + `fallbackExpr` fields (~50 LoC)
- Closure-bridge return path in `v3_hook.cc` (~90 LoC) and
  `v3ToTreeWalker`'s closure case (~135 LoC)
- `fiber.cc` + `bridge_yield.cc` + `fiber.hh` + `bridge_yield.hh`
  (~520 LoC including headers)
- `NIX_V3_FIBER_BRIDGE` opt-in + the fiber branch in
  `primV3CallBridge1` (~30 LoC)

**Total: ~2,800 LoC.**  Tier 4 (depends on `applyClosure` migration).

**Single handle system:** `v3BridgeClosures()` (grow-only,
`traceable_allocator`-rooted, no ABA defence) **dies**.
`EvalScope` + `ClosureHandle` (`ffi.cc:51-244`) is the production
path under target architecture.

**Fiber retirement:** under target arch, no call shape needs
fiber-bounded stacks.  v3 leaves call TW for `Fallible<T>` returns
(`readFile`, `parseStorePath`); they don't recurse back into v3.
TW callbacks via `applyClosure` run on the caller's stack and v3's
frame stack is heap-allocated.  **Delete `fiber.cc` +
`bridge_yield.cc` from default build** (gate behind
`-Dv3-fiber-bridge=disabled` for one release before deletion).

**The "10 callback sites" reality:** under target architecture, only
**6** TW-host primops have closure callbacks (the rest are pure-v3:
`map`/`filter`/`foldl'`/`genericClosure` already call v3
`callClosure` directly).  The 6 are:

1. `addToStore` PathFilter — `bool(string_view)`
2. `genericClosure` operator — already pure-v3
3. `derivationStrict` — no closure callback
4. `builtins.path { filter = ...; }` — curried `path: type: bool`
5. `builtins.fetchTree` `submodules` predicate
6. `builtins.filterSource` filter

**`builtins.path { filter }` is the cleanest first migration** —
single concrete callback, structurally well-bounded, ~3 days.  Once
it routes through `applyClosure`, the FFI is real and the bridge
deletion path opens.

---

## 3. FFI categories & speculative ABI (Agent 3)

The current FFI plan has 13 categories.  Under INVERSION, **11 of
13 are obsolete or cosmetic.**

**Drop entirely** (categories obsolete under v3-as-host):
- A (Parser) — TW parses; v3 gets `nix::Expr*` directly (~16 LoC)
- B (Symbol/Pos tables) — shared headers (~22 LoC)
- D (Network fetchers) — direct call (~15 LoC)
- E (Store ops) — direct `state.nixEvalState->store->...` (55 sites
  flat for 3 days; thesis falsified by data) (~50 LoC)
- F (Derivation construction descriptor) — TW-side (~80 LoC)
- "Eval entry" (`evalExpr`/`evalFile`) — v3 *is* the eval entry
- I (Settings) — v3 reads directly (~15 LoC)
- J (Logger) — direct `nix::Logger` (~5 LoC)
- L (StringContext) — re-exports `nix::ContextElem` (~10 LoC)
- M (BlockingFFI) — only used as decoration on D/E (~12 LoC)
- Plugin ABI Option B — zero non-`ffi.hh` references; speculative (~10 LoC)

**Drop unless wired in 4 weeks:**
- C (Filesystem I/O) — bodies exist in `ffi.cc:338-377`, **0 production
  routings**; primops still call `path.readFile()` directly.  Either
  migrate `primReadFile`/`primReadDir`/`primPathExists` by 2026-06-04
  or delete (~50 LoC).

**Scope down (keep minimal post-INVERSION FFI):**
- G (Closure / handle ABI) — keep `EvalScope` (simplified),
  `allocClosureHandle`, `isValid`, `lookupClosureHandle`,
  `applyClosure`.  Drop `applyClosureN`, `getClosureFormals`,
  `promoteToGlobal`, `releaseGlobal` unless wired in 4 weeks.
- H (EvalError / Fallible) — keep, needed for `applyClosure`'s
  return type.

**EvalScope simplification:** today's implementation is over-engineered
(atomic generation counter, thread-local scope chain,
mutex-protected `g_liveScopes`, packed handle encoding for ABA).
Under v3-as-host, v3 controls when handles become invalid (closure
registration → Evaluator destruction).  Collapse to a per-Evaluator
generation-tagged slot table.  ~50 LoC of complexity gone.

**Reframed `ffi.hh`:** today 532 LoC, ~38 callable declarations, 13
categories, 6 implementations, **0 production routings**.  Target:
**~120 LoC, ~5 callable declarations**, 1 category (Closure handle
ABI) plus the boundary-error types it needs.

> Headers without callers are not contracts; they are hopes.
> The post-INVERSION FFI deserves to be a small, real, used surface,
> not a 38-function aspiration that diverges further from reality
> every week.

---

## 4. STG bisect harness + identity-bug workarounds (Agent 4)

This entire layer exists because the **structural fix** to the
recurring identity-bug class hasn't landed.  Once `LexicalFromRef`
generalises #530 to the entire from-expr surface (REVIEW-09 §3,
~300 LoC structural change), all of the following retire:

**11 bisect-harness flags retire:**

- `NIX_V3_NO_STG`, `NIX_V3_STG`, `NIX_V3_STG_KEEP_HOOKS`
- `NIX_V3_NO_EARLY_PUBLISH`, `NIX_V3_EARLY_PUBLISH`,
  `NIX_V3_EARLY_PUBLISH_DBG`, `NIX_V3_EARLY_PUBLISH_ALL`
- `NIX_V3_NO_PARTIAL_BINDINGS_RECOVER`, `NIX_V3_DBG_PARTIAL_BINDINGS`
- `NIX_V3_PUBLISH_NON_REC_INIT`
- `NIX_V3_NO_BLACKHOLE_AS_VALUE`, `V3_DBG_BLACKHOLE_AS_VALUE`

**~290 LoC of workaround machinery retires:**

- `partialBindingsRegistry()` + thread-local map (~25 + 17 GC = 42)
- `publishToNearestBlackThunkFrame` body (~144)
- 7 publish-call sites + recovery in OP_REC_BINDING_SLOT_REF (~91)
- BlackholeError recovery + `clearBlackMarksOnException` registry erase (~46)

**~350 LoC of `lower.cc` heuristic stack retires:**

- `pushInheritFromCache` doc block + 8-reason `s_maxLevel=4` comment (~69)
- `s_stgMode`/`s_lambdaSkip`/`s_thunkifyAll`/`s_noThunkify` flags (~9)
- `isSelfDotPattern` + self-dot heuristic family (~52)
- `isComplexFromExpr` whitelist + #548 follow-on (~125)
- `s_dbgFires` / `s_fireLimit` / `s_fireSkipNth` diagnostics (~24)
- `V3_DBG_INHERIT_FROM_THUNK` block (~37)
- `NIX_V3_THUNK_CALL_ON_SELECT_VAR` gated #548 follow-on (~20)
- 4 inherit-from heuristic flags

**Structural items that KEEP:**
- `Tag::Bridge`, `Tag::Slot`, `OP_REC_SLOT_PUBLISH` (STG indirection)
- `Thunk::cell`, OP_RETURN cell-update (STG-8 — `stg_IND` analogue)
- STG-10 single-VM re-entry, `ScopedActiveV3VM`, `activeV3VM()`
- STG-13 native intrinsic dispatch
- STG-14a `tryDispatchTWLambdaInV3` (forward direction)

**Perf delta when publish-walk recovery is removed:** estimated
**5-15 % on attrset-heavy real workloads** (cardano-node, nixpkgs
hello.name).  The 2.4× compute-bound fib gap is owned by dispatch
overhead, not publish-walk — that gap is closed by computed-goto +
typed numeric opcodes (OPTIMIZER_REPORT #5 + #8).

---

## 5. IR / lower / emit reductions (Agent 5)

**`lower.cc` trajectory:**
- Today: 2728 LoC (was 2227 a few weeks ago; 24 commits in 48h
  per REVIEW-09 §4)
- After LexicalFromRef + heuristic-stack retirement: **~2520 LoC**
- After Phase 3 (deletes `subExprFuncs` + lambda-skip + force-hook
  origin tables): **~2350 LoC**
- After mechanical split (`lower_attrs.cc` + `lower_lambda.cc`):
  **~1500 LoC** ✓

**Critical sequencing:** do not refactor `lower.cc` before
LexicalFromRef lands.  The 24 recent commits sit precisely in the
heuristic-stack territory that's about to retire; a split before is
merge friction, a split after is a clean mechanical move.

**IR / closure field drops:**

| Field | Why droppable | Per-instance cost |
|---|---|---|
| `ir::Function::astLambda` (`void *`) | TW formals-bridge crutch | 8 B/Function |
| `LambdaDescriptor::astLambda` | Mirror of above | 8 B/descriptor |
| `ir::Module::varOrigins` + `recVarOrigins` | Force-hook cutover only | vector/Module |
| `ir::Module::recVarIds`, `recSlotVarIds`, `litBuiltinsVarIds` | `noUpvSrc` failure-mode handling | 3 vectors/Module |
| `ir::Module::subExprFuncs` | Force-hook cutover wiring | vector/Module |
| `LambdaDescriptor::forceCount` (mutable u64) | Diagnostic; one read site | 8 B + atomic-shaped write/force |
| `LambdaDescriptor::allocCount` | Diagnostic | 8 B/descriptor |
| `LambdaDescriptor::name`, `Function::name` | Diagnostic; move to side table | string/instance |

**Total per-descriptor savings: ~48 B/descriptor + 1
atomic-shaped write per force.**  The write is the load-bearing
cost; bytes are second-order under the planned Cheney nursery.

**Keep (cheap diagnostics with single readers):**
- `Thunk::forces` (4 B + 1 inc/force; useful for memoization-
  regression hunts)
- `posHandle` (load-bearing for runtime errors)
- `forceEmitSites` side table (zero hot-path cost)
- `AttrSelectIC` (live, hot; replaced by shapes when shapes land)

**Optimiser-pass survivability** under shapes + occurrence + selector
thunks + heuristic inliner:
- `constantFold` — survives intact
- `commonSubexprElim` — expanded (cross-block via use-def map)
- `elimRedundantForce` — **replaced** by demand analysis
- `inlineTrivialBindings` — **replaced** by heuristic inliner
- `fusePrimOpApps` — survives (orthogonal)
- `deadBindingElim` — simplified (2-pass with `OccKind::Dead`)

**Reserved opcodes** (`OP_NOP`, `OP_POP`, `OP_SWAP`, `OP_NEGATE`,
`OP_BRANCH_TRUE`, `OP_POS`): reclaim at v5 disk-cache schema bump.
6 bytes of opcode space.

**Add at next emit-touching sprint:**
- `OP_ADD_II/SUB_II/MUL_II/EQ_II` (OPTIMIZER_REPORT #8) — 3-5 % on
  numeric loops, ~200 LoC.

---

## 6. Tests + lode/ docs (Agent 6)

**~10 of 27 shell scripts retire (37 %):**

- **Drop after Phase 3** (5 scripts test `NIX_USE_V3=1` hook):
  `run-cutover-tests.sh`, `run-cutover-parity-tests.sh`,
  `run-on-demand-root-tests.sh`, `run-on-demand-root-shapes.sh`,
  `run-gate-removal-tests.sh`.
- **Drop after Phase 4** (3 scripts test bridge plumbing):
  `run-bridge1-shortcut-tests.sh`, `run-tw-lambda-bridge-tests.sh`,
  `run-lazy-bridge-arg-tests.sh`.
- **Drop after structural fix** (2 scripts test bug-class workarounds):
  `run-broader-thunkify-tests.sh`, `run-fix-inherit-from-self-tests.sh`.
- **Archive now** (3 wc38 bisect items per CLEANUP_AUDIT).

**Keep forever (12):** lang/laziness/disk-cache/forward-Bridge/lint/
safe-eval/C++ units.

**8 lode/ docs to attic now** (extending CLEANUP_AUDIT's 3):

- The 3 BENCH-2026-05-04 trio (already in CLEANUP_AUDIT).
- `FFI_PLAN_2026-05-06.md` + `*06b.md` — pre-INVERSION; categories
  A/B/F obsolete.  Add `STATUS: PAUSED — Phase 4 only` banner.
- `TEAM_A_B.md` — LoC claim wrong (2227 vs 2728); 2-week prep sprint
  never ran; INVERSION reshapes the team-split.  STALE banner.
- `OPTIMIZATION_PLAN.md` — own banner says superseded; chronological
  log preserved as historical artefact.

**5 lode/ docs to attic on milestone:**
- `CELL_UPDATE_AUDIT_2026-05-08.md`,
  `PUBLISH_RECOVERY_USE_AUDIT_2026-05-08.md` (after #498 / structural fix).
- `SLOT_AUDIT_HOOK_REENTRY_2026-05-08.md`,
  `EVAL_ORDER_DIVERGENCE_2026-05-08.md` (after Phase 3).
- `STG_INVENTORY_2026-05-09.md`, `STG_DEFAULT_ON_2026-05-09.md`
  (when project-memory absorbs the headlines).

**Keep as cautionary case studies:** `TRAFFIC-OWNERSHIP-REVIEW.md`,
`WC38_FIX_PLAN.md`, the contradicted reviews — they earn their keep
because they document predicted-vs-landed gaps that future reviewers
benefit from.

**Project-memory hygiene:** 7 stale memos identified, including
`project_v3_status.md` (pre-INVERSION cutover thesis),
`project_vm2_*` (project moved to v3),
`project_v3_primary_inversion.md` (predates `INVERSION_PLAN_2026-05-08.md`).
Recommend a single `project_memory_status.md` survey memo adding
STATUS lines to the seven, plus three explicit "supersedes" pointers.
Half a day of work prevents future reviewers from triangulating off
contradictory memos.

The vm2 trio (`vm2_status`, `vm2_review_session`, `vm2_optimizations`)
should move to `memory/attic/` — correct as historical record,
actively misleading when surfaced by `grep "v3 status"`.

---

## Cross-cutting themes

### A. Hook-mode + return-bridge are one architectural cut

Domains 1 and 2 are *one architectural commitment*: TW is the host;
v3 is a guest invoked via primop calls.  Inversion to v3-as-host
removes the *whole layer* — not 26 disconnected items, one system.
~7,000 LoC moves together as INVERSION Phases 3/4 land.

### B. Forward-direction Bridge plumbing survives intact

A consistent finding across Domains 1, 2, 3, 4: items that look
hook-mode but are actually forward-direction (`Tag::Bridge`,
`OP_FORCE` Bridge handler, `treeWalkerToV3` for non-closure shapes,
`tryBridgeAttrLookup`, `tryDispatchTWLambdaInV3`,
`ScopedActiveV3VM`, STG-10 single-VM, STG-8 cell-update) **survive
the inversion intact**.  Phase 2 actively uses them.  Future memos
should not lump these in with retirable hook-mode code.

### C. The structural-fix keystone

The single highest-leverage architectural action is **LexicalFromRef**
(per REVIEW_2026-05-09 §3): generalise #530's static lexical-with
chain to the entire inherit-from / let-rec from-expr surface.
~300 LoC structural change.  Subsumes #497, #528, #529, #498.
Enables **~640 LoC of cleanup in Domain 4** (heuristic stack +
publish-walk recovery + 11 env-var flags retire in cascade).

### D. The "headers without callers" pattern

Domain 3 (FFI) and parts of Domain 1 (sub-expr cache machinery)
share a pathology: declarations and bodies that exist for a planned
direction without a current consumer.  Specific examples:

- `applyClosure` — body exists, 0 production callers (only tests)
- `v3::readFile`/`readDir`/`pathExists` — bodies exist, 0 routings
- `Plugin ABI Option B` — types declared, no plugin
- The 11 obsolete FFI categories — declared, not implemented

The discipline answer: **headers without callers are not contracts;
they are hopes.**  Each accumulating week of "we'll wire it"
without action is evidence of mis-prioritisation.

### E. Conservatism vs opinion: both views correct

The cleanup audit (`CLEANUP_AUDIT_2026-05-09.md`) was correct to
recommend ~110 LoC deletable today and gate the rest on milestones.
This audit is correct to identify ~10,000 LoC of code on the wrong
side of the architectural line.  The reconciliation: **mark
everything in Tier 2-4 with `[[deprecated]]` or
`[[deprecated_in_target_arch]]` markers now**, even when the
deletion can't happen yet.  This makes the architectural intent
visible to every reviewer of a new patch, prevents new accretions,
and makes the eventual deletion mechanical.

---

## Tier-ordered action list

### Tier 0 — do today (zero risk, ~150 LoC + doc + markers)

1. **Mark all hook-mode-only symbols `[[deprecated]]`**: `v3EvalEntry`,
   the 4 Phase E lift gates, `willReturnClosure`,
   `tryDispatchFormalsLambdaBridge`, top-level Attrs/List
   short-circuit, the 6 fallback-reason codes.  Visible architectural
   intent.  ~30 marker insertions.
2. **Add `STATUS: PAUSED — Phase 4 only`** to `FFI_PLAN_2026-05-06.md`
   and `*06b.md`.  Add `STATUS: STALE` to `TEAM_A_B.md`.
3. **Move 8 docs to `lode/attic/`** (3 BENCH + 2 FFI_PLAN + TEAM_A_B
   + 2 stale-banner OPTIMIZATION_PLAN candidates).  Move 3 wc38
   bisect items to `test/attic/`.
4. CLEANUP_AUDIT Tier 0 deletions (~150 LoC) — `primop_canon.hh`,
   etc.
5. **Update `USAGE.md`** for the 77 missing flags + polarity errors
   (REVIEW_2026-05-09 §4).

### Tier 1 — at next disk-cache schema bump (v5)

6. Reclaim 6 reserved opcode bytes (`OP_NOP`, `OP_POP`, `OP_SWAP`,
   `OP_NEGATE`, `OP_BRANCH_TRUE`, `OP_POS`).  Strip enum entries +
   disasm cases + 80-line doc block.

### Tier 2 — when LexicalFromRef lands (the keystone)

7. Retire 11 STG bisect-harness flags.
8. Delete `partialBindingsRegistry` + `publishToNearestBlackThunkFrame`
   + recovery sites (~290 LoC).
9. Delete `lower.cc` heuristic stack (~350 LoC of executable + heavy
   comment).  `lower.cc` shrinks 2728 → ~2520.
10. Drop 2 tests (`run-broader-thunkify`, `run-fix-inherit-from-self`).
11. Archive 2 STG-era audits (`CELL_UPDATE_AUDIT`, `PUBLISH_RECOVERY_USE_AUDIT`).

### Tier 3 — after INVERSION Phase 3

12. Delete `v3EvalEntry` + downstream (~2,480 LoC).  `v3_hook.cc`
    shrinks to ~150 LoC.
13. Delete `subExprFuncs` + lambda-skip path + force-hook origin
    tables.  Delete `Function::astLambda`, `Module::varOrigins` etc.
    (Domain 5 IR field drops).
14. Drop 5 hook-mode tests.  Archive 2 hook-mode audits
    (`SLOT_AUDIT_HOOK_REENTRY`, `EVAL_ORDER_DIVERGENCE`).
15. **Mechanical split**: `lower.cc` → `lower.cc` + `lower_attrs.cc` +
    `lower_lambda.cc`.  Main file ≤ 1500 LoC.

### Tier 4 — after INVERSION Phase 4 (`applyClosure` migration)

16. Migrate 6 TW-side closure-callback primops through `applyClosure`.
    Start with `builtins.path { filter }`.
17. Delete `primV3CallBridge1` + `__v3_force_attr` + `__v3_force_list_elem`
    + tables + dispatch shortcuts + bridge guards (~2,800 LoC, including
    `fiber.cc` + `bridge_yield.cc`).
18. Delete `v3_hook.cc` entirely (rename / fold remaining
    `tryDispatchTWLambdaInV3` into `primops.cc`).
19. Drop 3 bridge tests.
20. **Reframe `ffi.hh`** from 532 → ~120 LoC (5 callable declarations).

### Tier 5 — keep forever (preservation inventory)

- Forward-direction Bridge runtime: `Tag::Bridge`, OP_FORCE Bridge
  handler, `forceValue` Bridge case, `tryBridgeAttrLookup`,
  `tryBridgeAttrHas`, `tryDispatchTWLambdaInV3`, `treeWalkerToV3`
  (non-closure shapes), `setNixEvalState`.
- Structural STG/V8 mechanics: `Tag::Slot`, `OP_REC_SLOT_PUBLISH`,
  `Thunk::cell`, OP_RETURN cell-update, `ScopedActiveV3VM`,
  STG-10 single-VM, STG-13 intrinsic dispatch, STG-14a forward
  dispatch.
- 4-way `AttrSelectIC` (until shapes land; then extends to shape PIC).
- `OP_GET_LOCAL_FORCE`, `OP_GET_UPVALUE_FORCE`, `OP_TAIL_CALL`
  superinstructions.
- `Thunk::forces` diagnostic (4 B + 1 inc; cheap, useful).
- `posHandle` (runtime errors).
- Cautionary lode docs: `TRAFFIC-OWNERSHIP-REVIEW.md`,
  `WC38_FIX_PLAN.md`, the contradicted reviews — preserved as
  predicted-vs-landed case studies.
- All language-semantics tests (lang, laziness, eval-fail).
- Disk-cache regression suite (CRIT-1).

---

## Verdict

**The codebase carries ~10,000 LoC of infrastructure that doesn't
fit the target architecture.**  This is not failure; it's
transitional state.  Most of it has known retirement milestones
(Phase 3 / Phase 4 / structural fix / v5 schema bump).  The
discipline gap today is that **none of it is marked**.

The single highest-leverage immediate action is **Tier 0 marker
insertion**: ~30 `[[deprecated]]` annotations on hook-mode-only
symbols, `STATUS:` banners on three lode/ docs, attic moves of 8
docs and 3 test files.  None of this is code change; it's
*intent-visibility* change.  Once the markers are in place, every
subsequent code review of a new accretion has a bright line: *"is
this depending on a `[[deprecated_in_target_arch]]` symbol?"*

The single highest-leverage architectural action is
**LexicalFromRef** (REVIEW_2026-05-09 §3 keystone): one structural
fix unlocks Tier 2 cleanups across Domains 4 and 5 in cascade.

The single highest-risk failure mode is **letting the FFI accrete
further**.  Each week of declarations-without-routings is more code
that future reviewers will believe is load-bearing.  REVIEW_2026-05-09
§5 already recommended pausing FFI categorical work; this audit
quantifies the gap (11 of 13 categories obsolete).  Pause now;
re-scope to Phase-4-minimal.

If the next sprint is **"insert deprecation markers, pause FFI,
ship LexicalFromRef, migrate `builtins.path { filter }` through
applyClosure,"** then ~3,000 LoC retires within 2-3 sprints and
the path to v3-native STG/V8 is concrete.  If not, the codebase
continues to accumulate transitional infrastructure that nobody
schedules for retirement and that future reviewers cannot
distinguish from load-bearing structure.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
