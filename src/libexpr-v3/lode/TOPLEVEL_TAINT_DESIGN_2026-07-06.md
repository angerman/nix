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

## ADVERSARIAL REVIEW VERDICT (2026-07-06) — policy-P re-run REJECTED; reroute to bitmask + manifest
The pre-build soundness review found the in-process re-run design (as written)
**serves a wrong drvPath**, and rejected it:
- **FATAL: single-bit taint.** `g_topLevelTaint` is ONE bool — it cannot tell
  WHICH impurity fired. Policy P perturbs only getEnv+currentTime; a readFile-
  derived result is tainted, the re-run reads the SAME (unperturbable) file →
  byte-identical → INSERTS → file changes → stale wrong result. Silent whole-eval
  miscompile.
- **Re-run is risky even fixed**: re-entrancy (depth guard), 2× cost on the
  tainted path, lowering-nondeterminism polluting the compare, taint-reset
  (run.cc:328) clobbering the base-run taint before capture, FFI leaves reading
  ambient state below the primop boundary, and 2-point clock-stability is not a
  purity proof.
- `--pure-eval`-default is sound ONLY WITH the axis-bitmask reject-set (pureEval
  does NOT block readFile of a mutable non-store path → still reachable → must be
  reject-set, not re-run).

**RECONCILED A1 PLAN (post-review) — strictly sound, no in-process re-run:**
1. **Axis bitmask (MUST-FIX #1, blocking):** convert `g_topLevelTaint` bool →
   `uint32_t` per-axis mask (TAINT_GETENV / TAINT_CURRENTTIME / TAINT_READFILE /
   TAINT_FETCH / TAINT_STORE / …); the 12 bump sites set their specific bit.
   Foundational for ANY precise policy. Over-approximation preserved (strictly
   safe). +/-/R: the existing TL2-TL5 still pass (they assert not-stale, which
   the reject-set upholds).
2. **Reject-set insert gate:** insert iff taint mask has NO bit outside
   `PERTURBABLE_MASK = {getEnv, currentTime}`; i.e. untainted OR only clock/env-
   tainted-AND-manifest-validated. Any readFile/readDir/pathExists/fetch/store/
   getFlake taint → hard REJECT (never insert). This alone is strictly sound
   (Alt-2 baseline) and can ship immediately (≈ today's coverage, but now
   PRECISE about which taint is recoverable).
3. **Offline manifest (Alt-1) to recover the clock-stable win:** the perturbation
   harness (bench/toplevel-cache-coverage.sh, already built + measured 98%) runs
   OFFLINE over the ≥50-pkg corpus and emits a signed manifest of clock-stable
   source-hashes. Production inserts a CLOCK-ONLY-tainted result iff its
   source-hash ∈ manifest; manifest-hash goes into the cache key; re-validate on
   nixpkgs bumps. This recovers the 98% corpus win with NO in-process re-run
   (= nix's flake-eval-cache trust model). SHIP gate (hello/firefox.drvPath +
   M5.name cross-process byte-id) achievable via manifest membership.
4. Policy-version bump v3-toplevel-v3 → -v4 on ship; ≥3 adversarial perturbation
   points offline; FFI-leaf ambient-read audit (writeDerivation embeds no
   timestamp — store paths are hash-based, confirm).
**DROP: the in-process re-run** (unsound as written, risky even fixed). The
manifest recovers the same win soundly.

## Build order for A1
1. [prereq] taint-all-impure-primops + version bump + tests + brute + commit.
2. perturbation harness (6-run: identical×2, getEnv-perturbed×2, currentTime-
   perturbed×2 via NIX_V3_FAKE_CURRENTTIME hook) + a >=50-pkg corpus manifest.
3. measure coverage% on the corpus (the SHIP go/no-go).
4. policy P (two-tier insert gate) + --pure-eval default + tests + brute.
5. darwin-4 GATE: cross-process byte-id HITs on hello/firefox.drvPath + M5.name,
   shadow mismatch==0 over the corpus, T_hit/T_eval<=0.20 → SHIP; else DEFER.

## VETTED IMPLEMENTATION SPEC (2026-07-06, opus adversarial soundness pass)
Grounded at HEAD (taint = per-axis mask primops.cc:6814; key `v3-toplevel-v3`
run.cc:1837; gate = reject-all-tainted run.cc:1871). The DECISIVE soundness
resolution + the buildable spec (a wrong top-level cache = SILENT WHOLE-EVAL
MISCOMPILE, so this was reviewed before any gate edit):

**Q1 getEnv (the hole found pre-build): RESOLVED.** Under `--pure-eval`/`--restrict-eval`
`primGetEnv` returns `""` UNCONDITIONALLY (primops.cc:1865-1872) → env-independent
BY CONSTRUCTION, so the key omitting env values is harmless. So:
`perturbable = TAINT_CURRENTTIME | (pure ? TAINT_GETENV : 0)`. Under `--impure`
getEnv reads the real env (not in key) → getEnv DEMOTED TO REJECT. The whole
manifest tier is honored ONLY under pure-eval (the manifest is generated/valid
only there); under `--impure` only untainted inserts.

**Q4 exact insert gate** (replaces run.cc:1871):
```
mask = topLevelTaintMask();
pure = ffi::pureEval(*state.nixEvalState) || ffi::restrictEval(*state.nixEvalState);
perturbable = TAINT_CURRENTTIME | (pure ? TAINT_GETENV : 0);
rejectBits = mask & ~perturbable;
insert  iff  rejectBits==0
        &&  ( (mask & perturbable)==0                              // untainted
              || (pure && manifestContains(manifestEntryId(...))) ) // blessed clock-stable
```
Order is load-bearing: **reject-bits FIRST** (readFile/fetch/store/impure-getEnv
dominate the manifest — TL10), **manifest LAST**, **manifestContains fails CLOSED**
(missing/unreadable/hash-mismatch/bad-signature → false).

**Q3 manifest id = SHA-256(keyBody)** where keyBody = the topLevelCacheKey bytes
MINUS the `"v3-toplevel-vN"` version tag (schema‖system‖NIX_PATH‖basePath‖source).
Refactor topLevelCacheKey → `keyBodyBytes()` helper so production lookup ==
manifest key BY CONSTRUCTION. Manifest blesses the EXACT (source,NIX_PATH,system,
basePath) tuple; coverage limit = only pre-blessed exprs get the clock-tainted win
(novel exprs fall to the sound untainted-only baseline — a coverage gap, not a
stale-result hole).

**Q5 key v3→v4** + fold `manifestContentHashHex()` (a manifest change invalidates
clock-tainted entries) + `codegenGateFingerprint()` (R4; currently ABSENT from the
top-level key — present only in the CU key primops.cc:7445; must be exposed to
run.cc). Fold both UNCONDITIONALLY (one key shape; lookup precedes eval so taint
isn't known at lookup).

**Q2 manifest certifies** currentTime-stability via >=3 ADVERSARIAL STRADDLING
clocks (not 2 — 2 can wrongly bless an `if currentTime>T` source; TL8). Extend
bench/toplevel-cache-coverage.sh to emit a SIGNED manifest of sources byte-stable
across all points; the generator's refuse-to-bless-unstable IS the sole soundness
gate for clock-tainted entries (runtime trusts the manifest). Add a getEnv-sentinel
pass as a generation-time leak cross-check.

**Q6 M5.name/A3: CONFIRMED** getFlake→TAINT_FETCH→reject until A3 keys the flake
lock. A3 must add to the key: resolved-NIX_PATH content-ids (replace the raw
NIX_PATH env string — the R2 mutable-channel gap), flake-lock rev/narHash, and
codegenGateFingerprint; then getFlake taint can demote reject→keyed. Until A3,
M5.name reject is SOUND. **A1 full SHIP gate is COUPLED with A3** for the M5.name
leg AND for mutable-NIX_PATH robustness (R2 ship-blocker: A1 on a mutable channel
is UNSOUND; pinned/immutable archive-URL NIX_PATH is fine → A1-alone can SHIP the
pinned hello/firefox.drvPath leg).

**Q7 tests (TL7-TL12, in --brute core), test hook `NIX_V3_TOPLEVEL_MANIFEST=<path>`
(inline retirement: retire when the production manifest path is wired):**
- TL7 (+) manifest-blessed clock-independent source caches+reuses byte-id (pure-eval).
- **TL8 (− CRUX, failing-first): a clock-DEPENDENT source (`if currentTime>1.5e9…`)
  wrongly blessed by a 2-clock manifest is ACCEPTED (red); the >=3-adversarial-clock
  generator REFUSES to bless it (green). Proves >=3 straddling points is load-bearing.**
- TL9 (−) getEnv under `--impure` rejected despite manifest (mode-dependent perturbable).
- TL10 (−) readFile hard-reject dominates a wrongful manifest bless (reject-bits-first).
- TL11 (−) v3 entry NOT served by v4 (version-tag keyspace partition).
- TL12 (R) manifest swap M1→M2 = miss then hit (manifest-hash-in-key invalidation).

**Residual soundness risks (ranked, with mitigations):** R1 FFI-leaf ambient reads
below the primop boundary (HIGH — audit the 11 bump sites + derivationStrict/
writeDerivation; getEnv-sentinel catches env leaks; pure-eval bounds the surface);
R2 mutable NIX_PATH (HIGH — deferred to A3, ship-blocker for mutable channels;
pinned OK); R3 tryEval taint-non-restore (LOW, safe over-approx); R4 codegen
non-determinism (MED — codegenGateFingerprint in key + shadow mismatch==0 gate);
R5 IFD reads (MED — go through readFile→TAINT_READFILE→reject, confirmed); R6
__currentTime in drv (NONE — writeDerivation embeds no timestamp, store paths are
content-hash; confirms the empirical 98% clock-stability is structural); R7
manifest fail-open (HIGH if mishandled — fail CLOSED + sign + verify + hash-in-key);
R8 corpus coverage gap (LOW — perf not soundness; ensure SHIP workloads are in corpus).

**SINGLE MOST IMPORTANT INVARIANT:** gate checks reject-bits FIRST, manifest LAST,
fails CLOSED, honors the manifest ONLY under `--pure-eval`; the manifest's validity
rests ENTIRELY on the offline generator's >=3-adversarial-point perturbation refusing
to bless any moving result. TL8 keeps that honest.

**BUILD STATUS:** spec vetted; mechanism NOT yet built. Remaining = keyBodyBytes
refactor + expose codegenGateFingerprint + v4 + manifest loader (fail-closed) +
Q4 gate + harness manifest-emit (>=3 clocks) + TL7-TL12 + `--brute` + shadow
mismatch==0 + darwin-4 T_hit/T_eval<=0.20 → SHIP (pinned .drvPath) / DEFER-to-A3
(M5.name + mutable NIX_PATH).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
