# v3 post-IFD-frontier roadmap (2026-08-09)

After the IFD frontier was measured to its end ("v3 is done" for the eval engine — every
eval-perf lever dead, deploy value = cache moat + CI integration), this is the roadmap of
four OUTSIDE-THE-BOX workstreams, each gated on a Phase-1 measurement. Successor context:
`project_v3_ifd_frontier_2026-08-08` (memory), `PROGRESSION_PLAN_2026-08-07.md`.

Companion design docs (this dir): `PLAN_TIERB_INCREMENTAL_EVAL_2026-08-09.md`,
`PLAN_TIERC_SHARED_ZYGOTE_2026-08-09.md`, `PLAN_TIERD_FUZZ_MONITOR_2026-08-09.md`,
`IFD_SOLVE_FINDINGS_2026-08-09.md`.

Discipline (all WS): empirical `nix eval` v3-vs-TW correctness (never code-reading); gate =
full `--brute` ALL GREEN + firefox drvPath byte-identical; CPU/RSS on darwin-4; git-note
every number; Rule 0 (a KILL / clean measurement is a deliverable); default-off flags with
inline retirement criteria; no push without explicit OK; STOP + report at each WS gate.
Materialization REJECTED (make the solve faster, don't check plans in). Dead eval-perf
levers off-limits.

## Build order
WS-0 → then WS-C (build) ∥ WS-IFD-SOLVE (farm A/B) ∥ WS-D (v2 spike) → WS-B (shadow-gate) last.

## Workstreams (Phase-1 measured 2026-08-08/09)

- **WS-C zygote** — base eval = **34–46%** of warm per-worker wall (K1 PASS; 2.64 s vs
  5.7–7.8 s HNE). Both levers largely built: Lever-1 (RO bytecode mmap) = WS-5 AOT
  (`aot_cache.cc`, 105 MB Shared_Clean); Lever-2 = `v3-eval --fork-worker`
  (`cli/v3-eval.cc:606`). NEXT: K2 dirty-page measurement (per-child Private_Dirty vs
  fresh), then port warm-parent-CoW-fork into nej + wire AOT deploy. KILL: per-child
  private >70% of fresh, base <25% on re-measure, or fork drvPath divergence. **BUILD FIRST.**
- **WS-IFD-SOLVE** — convert (nix-tools `plan-to-nix`) is free (0.15 s); the cost is the
  cabal Modular **solve** (`make-install-plan`), 24.6 s cold marginal (cardano-node 2051-pkg).
  Pipeline pins index-state + supports freeze (null default); `truncate-index` NOT wired.
  NEXT (farm): A/B 24.6 s baseline vs truncate-index-pruned vs freeze-fed. KILL a lever if
  <15% reduction; if both weak, document the solver ceiling + DEFER the faster-solver moonshot.
- **WS-D fuzzer** — naive dual-path generator = **0 divergences / 500** (common surface
  clean; expected). NEXT: coverage-directed v2 (steer to the ~185 uncovered primops) + the
  3-tier error oracle (throw-vs-succeed class-parity = zero-FP primary). Build the full
  framework (minimizer + fixture-emit + nej `NIX_V3_PARITY_SHADOW` monitor) ONLY if v2 yields
  real divergences; KILL it if v2 stays clean.
- **WS-B incremental eval** — highest ceiling, highest risk. Query node = the existing
  applied-cache key (capture-free, `vm_applied_cache.cc:187`) + content-hash + dep-sets +
  IFD-taint. GATE HARD: SHADOW-MODE ONLY first — measure the eval-cost-weighted pure
  (non-IFD) fraction + cross-process would-hit rate + ZERO false drvPath vs TW. KILL if the
  pure fraction is small (capture-free key admitted ~2–3% on hello; may miss the callPackage
  flood) OR any false drvPath (the moat-store bar). No build before the shadow number.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0

---

## GATE VERDICTS (2026-08-09) — all four workstreams

- **WS-0 durability** ✅ DONE (`1b3cb51a8`).
- **WS-IFD-SOLVE** 🔴 KILL (pipeline) + DEFER (moonshot) (`e651dfb2c`): truncate-index already
  wired; freeze = reuse subsumed by warm store; floor = cabal Modular solver (24.6s).
- **WS-D fuzzer** 🟢 GO + real bug shipped (`a2b335a68`): directed v2 caught + fixed
  `zipAttrsWith` fail-open (`--brute` 42/42). Framework productionization (minimizer +
  fixture-emit + nej `NIX_V3_PARITY_SHADOW` monitor) = recorded follow-on (touches nej).
- **WS-C zygote** 🟢 K2 PASS → GO; build SPEC'd (`a23849fef`). Build = nej worker-lifecycle
  restructure (Topology B) + AOT deploy wiring; gate = `--brute` under the forked pool
  (faithful on Linux/farm). Touches nej + deploy → needs OK before push.
- **WS-B incremental eval** 🔴 KILL: measured capture-free pure fraction <1% (hello 0.77%,
  HNE-aggregate 0.05%) — the hashable set = imports, ALREADY memoized by shipped LEVER-1 +
  import/bytecode caches; the 99%+ unhashable callPackage flood is the cost bulk and is
  structurally un-keyable. Marginal cross-process prize beyond shipped caches = negligible.
  (Cost-weighting inferred: hashable=imports=already-cached; not separately instrumented.
  Zero-false-drvPath already guaranteed by LEVER-1's shadow-compare, keyExceptionBail=0.)

**Analysis/gate phase COMPLETE for all four.** Remaining = the nej-touching BUILDS (WS-C
restructure; WS-D framework's nej monitor), which per the discipline ("no push without OK")
+ the Linux-farm gate need explicit go-ahead.
