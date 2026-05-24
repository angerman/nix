# V3 true-native roadmap — bridge elimination + over-forcing RCA — 2026-05-24

This plan addresses two coupled architectural problems that emerged
from today's haskell.nix-example real-test attempt:

  1. **Over-forcing**: v3-direct on haskell-nix-example takes >12 min
     vs TW's 2m46s cold / 1.7s warm.  v3 builds derivations TW does
     not need (python3, jq, apple-sdk, signing-utils, nuke-references,
     atf).  This is a 5-7× wall regression AND a build-time
     regression (v3 produces extra .drv files that TW does not).

  2. **Future-true-native**: the v3→TW bridge surface is still wide
     (~14 call sites in primops.cc + emitTreeAttrs in v3_call_flake.cc).
     The V3-NATIVE constraint per CLAUDE.md §1 says TW is permitted
     ONLY at FFI leaves.  Several current bridge sites are NOT FFI
     leaves — they are evaluation crossings.

The two problems are **likely the same root cause**: when v3 hands a
value to TW (via `v3ToTreeWalker`) or TW invokes a v3 closure (via
`__v3_call_bridge_1`), the strictness boundary is mismatched.  Each
crossing causes either:
  (a) v3 to force a value lazily-handled by TW; or
  (b) TW to force a value v3 would have left lazy.

Both result in over-eager builds, which is what we observe.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.

## What we know today (2026-05-24, post-commit `04f88f7a0`)

### Bridge surface

From `primops.cc` (per Explore agent survey):

  | Category | Sites | Status |
  |----------|-------|--------|
  | Truly eliminable (path-coercion in readDir/import) | 6 | unfixed |
  | Task #660 retirement (force-attr, call-bridge-1, force-list-elem) | 4 | pending |
  | Architectural FFI leaves (fetchers, builtins.path, fetchFinalTree) | 4 | keep |

Plus `v3_call_flake.cc` keeps `emitTreeAttrs` (justified: hash/timestamp
formatting) and `fetchFinalTree` (rare fallback).

### Prior investigations on over-forcing (#754 / #755 / #756 / #757 / #757c)

  * `#754` (commit `aadc35ae5`): cardano-node M5 ACHIEVED **with TW
    callFlake bridge** — i.e. the resolution was the TW workaround,
    not full v3-native.  Wall 10.2s v3 vs 7.97s TW under bridge.
  * `#755-#756`: infinite recursion under native callFlake on
    cardano-node fixed via defaultExpr hoisting (`d9d3eed85`).
  * `#757`: kMaxIndirectionChase raised 4096 → 100 000 + V3_DBG_CHASE.
  * `#757c` (`e98c6f755` + `0c7c4f191`): primReadFile string-context
    + unsafeDiscardStringContext parity fixes.  Achieved cardano-node
    M5 v3-NATIVE byte-identical to TW (31.3s v3 vs 14.9s TW).

These resolved CARDANO-NODE M5.  haskell.nix-example is a SEPARATE
workload class (heavier IFD, more aggressive module-system use) and
is NOT covered by those fixes.

### Today's empirical evidence

  * haskell-nix-example via v3-direct (commit `04f88f7a0` defaults-on):
    - Hits `v3ToTreeWalker: <formals> closure cannot bridge`
      (FIXED for blocking by lifting refusal).
    - Then progresses past `cabalProject` option evaluation.
    - Starts building: signing-utils, nuke-references, remove-references-to,
      python3-3.13.7, jq-1.8.1, atf-0.23, apple-sdk-11.3, apple-sdk-14.4.
    - Killed at 12 minutes still building.
  * TW on same workload: 2m46s cold (builds plan-to-nix outputs only),
    1.7s warm (no extra builds; just reads cached drvPath).
  * The DIVERGENCE is in which derivations get realised.  v3 over-eagerly
    forces something that triggers builds TW does not need.

### Why this matters

Until this is resolved, v3-direct cannot be a viable replacement for
TW on the v3 user-base most likely to benefit (haskell.nix, IFD-heavy
workflows).  The 5-7× wall regression is the OPPOSITE of v3's value
proposition.

## Architectural vision (where we want to land)

Long-term V3-NATIVE end state:

```
                  ┌──── nix CLI ──── user input
                  │
                  ▼
  ╔═════════════════════════════════════════════════╗
  ║                  v3 VM                          ║
  ║  - all evaluation                                ║
  ║  - all closures, attrsets, lists                 ║
  ║  - all primops EXCEPT FFI leaves                 ║
  ║                                                  ║
  ║  ╔═══════════════════════════════════════════╗  ║
  ║  ║  FFI leaves (well-defined boundary)       ║  ║
  ║  ║                                            ║  ║
  ║  ║  • store operations (libstore)             ║  ║
  ║  ║  • flake ref / fetch (libfetchers)         ║  ║
  ║  ║  • file I/O (readFile, readDir, hashFile)  ║  ║
  ║  ║  • parser entry (parseFromString)          ║  ║
  ║  ║                                            ║  ║
  ║  ║  ALL of these return DATA (paths, hashes,  ║  ║
  ║  ║  bytes), NOT v3 Values that need eval.     ║  ║
  ║  ╚═══════════════════════════════════════════╝  ║
  ╚═════════════════════════════════════════════════╝
```

Specifically:
  * **No v3→TW closure invocation**: `__v3_call_bridge_1` is retired.
    All closure calls go through v3's `callClosure`.
  * **No TW→v3 attr lookup**: `__v3_force_attr` is retired.  v3
    attrsets are read by v3 only; their attrs are extracted v3-native.
  * **No TW→v3 list indexing**: `__v3_force_list_elem` retired.
  * **TW used for**: parsing inputs, store I/O, flake fetchers,
    derivationStrict at the very end (or v3-native fully).

## Hypothesis space for the haskell.nix over-forcing

H1. **Bridge causes strictness asymmetry**: TW invokes a v3 closure
    via `__v3_call_bridge_1`.  v3's `callClosure` forces formals more
    eagerly than TW's `callFunction` would.  This forces module-system
    options that TW left lazy.

H2. **Stage 4 strictness over-fires**: `applyStrictnessAtCallSites`
    inlines `Force{v}` → `VarRef{v}` based on conservative WHNF
    analysis.  On haskell.nix's overlays, this may force a thunk
    that TW left lazy.

H3. **primDerivationStrictNative forces too much**: when constructing
    a derivation, the native path forces attrs needed for drvPath
    (`name`, `system`, `builder`, `args`, `outputs`).  But also forces
    something extra (a `passthru` entry, an unused output attr).

H4. **callFlakeV3 over-forces flake outputs**: per #754's M5 history
    + `Explore` agent's note: "v3-native callFlake evaluates these
    outputs' drvPath attributes eagerly (during module-system
    assembly)".

H5. **Phase D/E barrier interaction**: write-barrier or scavenger
    causes a thunk to "wake up" earlier than its body would naturally
    force it.

H6. **Bytecode lowering eagerness**: opt_const_fold or similar
    optimisation eagerly evaluates a sub-expression that TW would
    leave as a thunk.

Each hypothesis is testable.  The plan below proves or kills each.

## Phased plan

### Phase A — Instrumentation & Measurement (~2 days)

Goal: collect ground-truth data about WHERE v3 over-forces.  No
guessing — measure first.

  * **A1**: Add per-call-site counter for `v3ToTreeWalker`.  Attribute
    each invocation to its caller (primDerivationStrict, primImport,
    primReadDir, primV3CallBridge1, primV3ForceAttr, etc.).  Dump
    under `NIX_VM_STATS`.  Cheap (one atomic increment per call).
  * **A2**: Add per-derivation-realisation counter.  Hook
    `realisePath` and `realiseContext` in v3's bridge to record every
    store path realised during eval.  Dump path list under
    `NIX_V3_REALISATION_TRACE=1`.  This gives us the "v3 built X but
    TW didn't" diff directly.
  * **A3**: Differential trace harness.  Run haskell-nix-example
    under TW with `NIX_TRACE_EVAL=1` (existing); run under v3 with
    `NIX_TRACE_EVAL=1` + new realisation trace.  Diff the realisation
    lists.  First derivation that's in v3's list but not TW's is the
    over-forcing site.
  * **A4**: Document the divergence.  Where in eval did v3 fork from
    TW?  Which expression was being evaluated?

### Phase B — Hypothesis Triage (~2-3 days)

Goal: kill the wrong hypotheses, identify the correct root cause.

  * **B1**: Test H1 (bridge strictness asymmetry).  Add a debug gate
    that makes `primV3CallBridge1` thunkify all args before
    dispatching to `callClosure`.  If this eliminates the extra
    builds, H1 is confirmed.  If not, H1 is killed.
  * **B2**: Test H2 (Stage 4 over-fires).  Add a debug gate that
    disables `applyStrictnessAtCallSites`.  Re-run.  If extra builds
    persist, H2 is killed.
  * **B3**: Test H3 (derivationStrictNative).  Force fallback to TW
    bridge via `V3_DRV_NO_NATIVE=1` (if exists; else add).  If extra
    builds disappear with native disabled, H3 is the culprit.
  * **B4**: Test H4 (callFlakeV3 eagerness).  Re-run with
    `NIX_V3_NO_NATIVE_CALL_FLAKE=1` (TW callFlake bridge).  If extra
    builds disappear, H4 is the culprit (and the M5 workaround
    extends to this case).
  * **B5**: Test H5 (Phase D/E).  Re-run with
    `NIX_V3_NO_PHASE_D=1`.  If extra builds disappear, H5 is the
    culprit.
  * **B6**: Test H6 (optimisation).  Re-run with all optimisations
    disabled (`NIX_V3_NO_OPT=1` if exists; else gate per-pass).  If
    extra builds disappear, H6 is the culprit.

### Phase C — Localise & Bisect (~2 days)

Goal: narrow down to the exact code path that over-forces.

  * **C1**: Construct minimal reproducer.  Find the smallest workload
    that triggers ≥1 extra build (using nixpkgs bisection per
    LESSONS §4.8).  Likely 50-200 lines.  Avoid haskell.nix
    dependency; use synthetic structurally similar code.
  * **C2**: Bisect within the minimal reproducer.  Strip features
    until extra builds disappear.  Identify the AST shape that
    triggers it.
  * **C3**: Map AST shape to v3 emit + run code path.  Set
    breakpoint or `V3_DBG_*` traces.  Verify the over-forcing site.

### Phase D — Fix Over-Forcing (~1 week)

Goal: implement the fix targeted at the root cause.

  * **D1**: Implement the fix at the localised site.  Likely scope:
    100-500 LoC, depending on the cause.
  * **D2**: Validate on minimal reproducer — no extra builds.
  * **D3**: Validate on haskell-nix-example — TW vs v3 builds the
    same set of derivations.  Wall ≤2× TW (per ≤2× criterion).
  * **D4**: Re-run lang + property + core test suites.
    Zero-regression required.

### Phase E — Bridge Elimination (~1-2 weeks, gated on D)

Goal: replace remaining non-FFI-leaf bridges with v3-native paths.

  * **E1**: Eliminate `primReadDir` / `primImport` realisePath
    bridges.  These call TW's `realisePath` (an FFI leaf for build
    + path), but the COERCION (attrset → string with context, etc.)
    is currently v3→TW.  Push the coercion v3-native; only call TW's
    `realisePath` with the resolved string.
  * **E2**: Eliminate `v3ToTreeWalker` eager-bridge for small lists/
    attrsets (lines 4786, 4901).  These are pure structural
    transforms, no FFI.  Replace with v3-native zero-copy.
  * **E3**: Retire `primV3ForceAttr` / `primV3CallBridge1` /
    `primV3ForceListElem` (#660).  Requires every TW→v3 boundary
    case to have a v3-native alternative.  Likely depends on D.
  * **E4**: Audit final FFI surface.  Document each remaining
    `v3ToTreeWalker` call site with rationale.  Target ≤4 sites:
    realisePath, derivationStrict (if not native), fetch*, builtins.path.

### Phase F — Validation & default-on for residuals (~3-5 days)

Goal: ship the cumulative work.

  * **F1**: haskell-nix-example end-to-end via v3-direct: wall ≤4×
    TW (cold), ≤2× TW (warm).
  * **F2**: Full `--full` test suite: 79 PASS / 0 FAIL (after
    addressing the pre-existing macOS test-script noise).
  * **F3**: Cumulative TW-bridge call-site count: at least -50 %
    vs current.  Document in updated FFI_AUDIT doc.
  * **F4**: Phase 4b retirement criterion at +30 days nightly CI
    without regression.

## Success criteria (binary)

For Phase D (root-cause fix):
  * ✓ v3 on haskell-nix-example builds the SAME set of .drv files as
    TW (no extra realisations).
  * ✓ v3 wall ≤4× TW on haskell-nix-example cold.

For Phase E (bridge elimination):
  * ✓ `git grep v3ToTreeWalker` returns ≤6 hits in primops.cc (4 FFI
    leaves + forward decls).
  * ✓ `primV3CallBridge1`, `primV3ForceAttr`, `primV3ForceListElem`
    are deleted from src/.
  * ✓ TW v2 bytecode VM (`src/libexpr/`) reference can be retired
    (it was per #735; this confirms residuals).

## Risks & alternatives

### Risk 1 — Root cause is in TW, not v3

Possible: TW's lazy semantics may rely on a side-effect that v3 doesn't
replicate (e.g. a force-cycle or memo).  In that case, "fixing v3"
means adding the same memo / cycle.  Phase A's diff harness will
expose this.

### Risk 2 — The bridge IS the right design

If H1 is confirmed (bridge strictness asymmetry IS the root cause),
the V3-NATIVE goal of eliminating the bridge is the correct fix.
But if the bridge is needed for soundness (e.g. some TW primop CAN'T
work without the bridge), Phase E is bounded by what's actually
eliminable.

### Risk 3 — Cardano-node regression

The defaults-on `NIX_V3_NO_REFUSE_FORMALS_BRIDGE=1` was validated
on cardano-node (15/15 callFlake sweep PASS) BUT not on the heavier
M5 workload.  Each fix in Phase D must re-test cardano-node M5
byte-identical to TW (per #757c reference).

### Alternative — accept the bridge, fix only the over-forcing

If Phase D fixes the over-forcing AND the bridge isn't measurably
expensive on real workloads, we could STOP at Phase D.  Phase E
becomes opt-in / future work.  Decision deferred until B6 data.

### Alternative — accept v3-direct as "advanced mode"

If the haskell.nix workload class proves architecturally intractable
under v3-NATIVE (e.g. requires deep TW co-operation), document this
and direct haskell.nix users to opt-out of v3-NATIVE.  Last-resort
position; not the working assumption.

## Operational notes

### Bench harness needed

Phase A's instrumentation needs to emit per-realisation traces that
diff cleanly between TW and v3.  Existing `NIX_TRACE_EVAL` may not
cover realisations; A2 adds the missing instrument.

### Lode discipline

This investigation should produce ONE living document
(`V3_TRUE_NATIVE_RCA_*.md`) updated as each hypothesis closes.  No
intermediate `_findings_v1.md`, `_findings_v2.md`.

### Falsification rule (Rule 0)

Every commit in Phase B/C/D must answer "what hypothesis does this
kill?"  If a fix doesn't kill a hypothesis, it doesn't merge.

## Cross-references

  * `lode/DEFAULT_ON_VALIDATION_2026-05-24.md` — the validation that
    landed the new defaults
  * `lode/PHASE_4B_*_2026-05-24.md` — Phase 4b validation arc
  * `lode/FFI_AUDIT_2026-05-20.md` — prior FFI surface inventory
  * Memory: `project_741_phase4b_validation.md`, `project_792_793_default_on.md`
  * Commits: `aadc35ae5` (#754 M5 ACHIEVED via TW bridge),
    `e98c6f755`/`0c7c4f191` (#757c readFile fixes), `d9d3eed85`
    (#756 defaultExpr hoist)
