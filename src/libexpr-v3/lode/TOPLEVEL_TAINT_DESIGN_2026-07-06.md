# A1 precise-taint design decision — empirical-corpus wins (2026-07-06)

Design fan-out (3 opus agents) for the top-level cross-process cache's insert-gate
soundness mechanism, reconciled adversarially. Grounded at HEAD de6ceb4d5.

## The problem
The top-level result cache (run.cc:1799/1822 key, 1867 insert-gate) memoizes the
whole-eval WHNF result cross-process. Today's taint is a CONSERVATIVE GLOBAL bool
`g_topLevelTaint` (primops.cc:6799-6802) bumped at exactly TWO sites — getEnv
(1855) + currentTime (3919). It OVER-REJECTS: nixpkgs calls currentTime in
result-irrelevant branches, so hello/firefox.drvPath are tainted=1 and NEVER
cache despite being deterministic (v1 shadow: mismatch==0). Goal: cache iff the
impurity does NOT reach the serialized result.

## Two approaches evaluated
### (i) Taint-bit-on-Value (precise data-flow) — REJECTED
- NO free bit in the 8B NaN-boxed Value (fully spoken for; pointers use all 48
  payload bits, floats are raw doubles) → taint must live on stable GC CELLS
  (Thunk flag bit THUNK_TAINTED=1<<3 is free; but containers need a side-set with
  GC-evac relocation = the missed-root/UAF class).
- **FATAL: pure data-flow taint is UNSOUND** — it misses CONTROL-FLOW laundering
  (`if getEnv "X" == "root" then "a" else "b"` — the condition is impure, the
  result differs across env, but neither branch value is individually tainted).
  Closing it needs IMPLICIT-FLOW taint (taint any branch whose condition is
  tainted), which OVER-taints: for nixpkgs (currentTime in conditions
  pervasively) it would RE-INTRODUCE the over-rejection we're fixing. So the
  "precise" approach, made sound, is no more precise than today's global bool.
- Cost: ~3-4 weeks; PRIMOP_IMPURE flag exists (primop.hh:373) but set on ZERO
  primops → from-scratch audit of all 131 primops; hot-path shadow-stack is a
  Rule-0 perf-regression risk; GC-relocation of container taint is a UAF risk.

### (ii) Empirical-corpus (policy P) — CHOSEN
- Does NOT analyze flows; MEASURES whether the result is byte-stable under
  PERTURBATION of the impurity sources (getEnv → sentinels, currentTime →
  advanced clock via a test hook). A result byte-identical across the perturbed
  runs is genuinely pure w.r.t. those axes → cache it. This SIDESTEPS the
  control-flow problem entirely (it observes the result varying, not the flow).
- Runtime policy P (replaces the run.cc:1867 gate): untainted → insert; TAINTED
  → re-run once in-process under a perturbed impurity env, insert ONLY if the
  serialized result is byte-identical. Reuses the existing shadow machinery
  (topLevelCacheShadow, byte-compare, TopLevelCacheStats).
- Coverage: **~95-100%** of the .drvPath/.name/.outPath corpus (nixpkgs recipes
  are pure; currentTime is in result-irrelevant branches) vs **~0%** for taint-bit.
- Soundness: default P ON only under `--pure-eval` (impurity surface is
  store-content-addressed-bounded ≈ EXACTLY nix's own flake eval-cache trust
  contract); opt-in under `--impure`. Residual risk = impurity axes neither
  tainted nor perturbed → mitigated to an ENUMERABLE obligation (taint/perturb
  every impure primop) rather than taint-bit's UNBOUNDED per-Value obligation.
- Effort: ~5-7 days, front-loaded on the perturbation harness so coverage% is
  known before policy P is built.

## Decision & rationale
**Empirical-corpus (policy P), default-on under `--pure-eval`.** It is cheaper
(~1/4 the effort), gets ~95-100% coverage the taint-bit forfeits, and is MORE
sound on the control-flow case (by measurement, not by an over-tainting flow
analysis). The taint-bit is DEFERRED as an unattractive "provable" path whose
sound form regresses to over-rejection.

## PREREQUISITE (both approaches need it — build FIRST): taint all impure primops
Confirmed by BOTH design agents: taint today covers only getEnv+currentTime.
readFile/readDir/pathExists/readFileType/hashFile/fetchTree/fetchGit/
fetchTarball/getFlake/storePath/getContext/exec + IFD-derived reads do NOT bump
taint — a silent-wrong-result hole for ANY caching policy (an un-tainted
impurity reaching the result is served stale cross-process). Extend
topLevelTaintBump to the full impure-primop set (+ tryEval body taint + the
existing ifdProbeWithCtx discriminator for IFD). This is the SOUND-DIRECTION
first step (makes the cache MORE conservative → strictly safe; policy P then
recovers the tainted-but-stable coverage). +/-/R: + a pure result still caches;
- readFile/readDir/pathExists/fetch reaching the result → tainted, not cached +
cross-version not-stale; R failing-first TL cases. Bump the cache-policy version
(run.cc:1833 v2→v3) so older-binary entries never collide.

## Build order for A1
1. [prereq] taint-all-impure-primops + version bump + tests + brute + commit.
2. perturbation harness (6-run: identical×2, getEnv-perturbed×2, currentTime-
   perturbed×2 via NIX_V3_FAKE_CURRENTTIME hook) + a >=50-pkg corpus manifest.
3. measure coverage% on the corpus (the SHIP go/no-go).
4. policy P (two-tier insert gate) + --pure-eval default + tests + brute.
5. darwin-4 GATE: cross-process byte-id HITs on hello/firefox.drvPath + M5.name,
   shadow mismatch==0 over the corpus, T_hit/T_eval<=0.20 → SHIP; else DEFER.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
