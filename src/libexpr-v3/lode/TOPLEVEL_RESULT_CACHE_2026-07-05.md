# Top-level result cache — "#741 done at the right boundary" (2026-07-05)

## Investigation verdict (why #741 delivers 0 speedup)
Mapped all of #741 (tasks #741-A/B).  Every hook does its lookup AFTER
`forceDeep(inputs)`:
- evalResultCache (primops.cc:5439/5695): key = canonicalHash of the FORCED
  derivation input attrset → must evaluate all inputs to compute the key.
- drvHashCache (primops.cc:5277): key = `drvPathS`, the OUTPUT of
  `writeDerivation(drv)` → drv is built from forced inputs → key known only
  AFTER the expensive work.

So a hit skips only the ~30-50µs libstore tail; the ~0.44s of input evaluation
is already spent.  Worse, the SQLite lookup (~60µs) EXCEEDS the 30-50µs saved
→ NET NEGATIVE (measured: cold DISK+ACTIVE +71%, warm +5.6%).  My earlier #2
"store KILL" (T_hit/T_eval=1.00) MEASURED THIS mis-keyed cache — an INVALID
proxy for the store concept.

**The problem is the BOUNDARY, not the lookup mechanism.**  The map's "use
mmap instead of SQLite" only rescues the low boundary to ~break-even (saves
30-50µs faster).  Raising the boundary to the TOP LEVEL — key = expression
identity + input pins, computable BEFORE any eval — makes a hit skip the WHOLE
0.60s.  There, even SQLite's 60µs is negligible: T_hit/T_eval < 0.002 ≪ 0.20.

## Design — top-level result cache
Hook: `runRootExprModule` (run.cc:317, `out.value = run(*out.cu)`).  The
top-level expr's WHNF result.  For scalar/string projections (`.drvPath`,
`.name`, `.outPath` — the user's firefox/M5/HNE workloads) the WHNF value IS
the final answer and value_serialize round-trips it; attrset/deep results
where value_serialize throws (unforced thunks) are a natural BYPASS (v1 caches
only serializable WHNF results).

KEY (soundness-critical — a wrong top-level result is a silent whole-eval
miscompile).  Must capture EVERY input the result depends on:
  SHA256( kEvalResultSchemaVersion ‖ nixVersion ‖ codegenGateFingerprint ‖
          builtins.currentSystem ‖ NIX_PATH-resolved-fingerprint ‖
          impure-mode-flag ‖ basePath ‖ source-expr-bytes )
The NIX_PATH-resolved-fingerprint = the resolved content id of each search-path
entry (e.g. the <nixpkgs> archive rev / store narHash) — resolving a search
path is a cheap store lookup, NO eval.  So the whole key is computable BEFORE
`run()` → an ACTIVE hit skips the entire eval.

TAINT (the hard soundness part).  The cached result must be a PURE function of
the key.  Impure inputs NOT in the key (builtins.currentTime, getEnv, readFile/
readDir/pathExists of non-store mutable paths, IFD-derived values, __currentTime)
must POISON the entry (skip insert).  v1 approach: SHADOW-FIRST reveals what
needs tainting empirically (below) before any taint code is written.

## Build order (SHADOW-FIRST — mirrors #741's own sound discipline)
1. **v1 SHADOW** (this step, gated NIX_V3_TOPLEVEL_CACHE=shadow, default off):
   after run(), if the result is serializable WHNF, compute the key, look up;
   ALWAYS run (no short-circuit) and COMPARE cached-vs-fresh byte-identically,
   counting mismatch.  This MEASURES (a) the hit rate and (b) soundness — a
   nonzero mismatch on a workload reveals exactly which impure input the key
   misses (→ what to taint).  ZERO risk (never reuses).
2. Measure hit + mismatch on hello/firefox/M5/HNE cross-process + T_hit(active-
   dry-run: key-compute + lookup + deserialize, without returning).
   PRE-COMMITTED gate: SHIP-to-ACTIVE only if mismatch==0 across all workloads
   AND T_hit/T_eval ≤ 0.20.
3. **v2 TAINT** from the shadow mismatches: poison entries whose eval touched an
   impure builtin (a taint flag bumped by the impure primops).
4. **v3 ACTIVE** (skip-on-hit) — only after shadow mismatch==0 with taint on.
   Byte-id ladder cached==fresh==golden; full --brute; cross-process T_hit gate.

## v2 TAINT finding (2026-07-05) — conservative global-taint is SOUND but OVER-REJECTS
Built a per-eval impurity taint (topLevelTaint{Bump,Reset,Tainted}): getEnv +
currentTime bump it; the shadow skips insert/compare when tainted; reset right
before the module's run() (AFTER the primop installer, which itself calls
currentTime — else EVERY eval, even `"abc"`, is falsely tainted; fixed).

MEASURED after the installer-taint fix:
- `"abc"` (pure): tainted=0 → caches (hit, mismatch=0).  Correct.
- `builtins.seq builtins.currentTime "abc"`: tainted=1.  Correct (genuinely
  calls currentTime), though the RESULT ("abc") is deterministic.
- **hello.drvPath: tainted=1** — nixpkgs GENUINELY calls `builtins.currentTime`
  during the eval (in a result-IRRELEVANT branch: v1 shadow proved the drvPath
  is deterministic, mismatch=0).  So the CONSERVATIVE global taint (taint if an
  impure primop is CALLED ANYWHERE) OVER-REJECTS the real workloads: nixpkgs
  pervasively calls currentTime in branches that don't flow into the result.

CONCLUSION: a global "impurity called" flag is sound but useless (rejects the
very workloads the cache targets).  A sound AND useful top-level cache needs to
distinguish "impurity called" from "impurity IN THE RESULT" — i.e. PRECISE
DATA-FLOW taint: propagate an impure-derived bit through Values, and taint only
when the serialized RESULT carries it.  That is a substantial feature (a taint
bit on Value / per-thunk provenance).  ALTERNATIVE: empirical-corpus approach —
ship ACTIVE for results that shadow-validate mismatch==0 across a wide corpus +
re-validate on nixpkgs bumps, accepting a bounded residual risk (what nix's own
eval-cache effectively does by keying on the flake lock and trusting purity).

STATUS: v1 shadow + v2 conservative taint are the SOUND FOUNDATION (committed,
gated-off).  The precise-taint OR empirical-corpus decision is the next careful
phase (the cache is PROVEN viable — T_hit≈0, hello mismatch=0 — the remaining
work is the soundness-vs-hit-rate mechanism).

## v3 ACTIVE (skip-on-hit) built + #2 GO verdict (2026-07-05)
Phase-1 ACTIVE shipped (gate NIX_V3_TOPLEVEL_CACHE=active|1, default-OFF): a
pre-run lookup at the outermost runRootExprFromString (keyed on inputs known
BEFORE parse) skips the WHOLE pipeline on a hit, returning the deserialized
WHNF result on a minimal CU.  SOUND: only UNTAINTED results are inserted
(post-run, taint-gated), so any cached entry is a pure function of the key.

#2 GO — corrected falsifier (replaces the RETRACTED T_hit/T_eval=1.00, which
measured #741's mis-keyed drv-hash cache): 12M untainted fold, laptop —
  T_eval(off) = 4.50s ; T_hit(active, skip) = 0.20s ; **T_hit/T_eval = 0.044**
  ≪ the 0.20 gate → GO.  The store concept WINS at the right boundary.

SOUNDNESS bug found + fixed (a real silent-wrong-result): a pre-taint "v1" key
getEnv entry was served after taint landed (cross-version cache poisoning).
FIX: the key prefix encodes the CACHE-POLICY VERSION (v1→v2 = taint on getEnv/
currentTime); bump it on any key/taint change.  Regression: TL2/TL3 in
test/run-toplevel-cache-tests.sh (fresh-cache getEnv-not-stale + currentTime-
not-frozen).  Verified: fresh cache → getEnv tainted (inserts=0), FOO=bbb→bbb.

PHASE-1 LIMITATIONS (documented; phase-2 work):
- Taint OVER-REJECTS the nixpkgs workloads (hello/firefox call currentTime in
  result-irrelevant branches) → .drvPath/.name don't cache yet.  Needs PRECISE
  data-flow taint (taint only if the impure value reaches the result) — the win
  set today is untainted evals (pure computation, non-impure-touching derivs).
- Fixed per-process floor ~0.09-0.20s (startup + registerBuiltinPrimOps) → the
  net win needs T_eval > ~0.45s.
- v1 key uses the NIX_PATH ENV STRING (sound for immutable pins; a mutable
  channel symlink is a gap) → opt-in, pinned-inputs-only.
So #2 = GO + phase-1 ACTIVE built (default-off), with precise-taint + resolved-
NIX_PATH-content keying as the phase-2 path to cover the nixpkgs workloads.

## Relation to prior work
- This is the CORRECT-boundary version of what #2 (RESULT_STORE) reached for;
  #2's KILL is RETRACTED (it measured the mis-keyed #741 drv-hash cache).
- Distinct from the in-memory applied cache (#1, shipped default-on): that
  memoizes `(import f) args` graphs in-process; this memoizes the TOP-LEVEL
  forced result cross-process.  Complementary.
- Reuses the existing substrate: value_serialize (forced-WHNF round-trip) +
  disk_cache EvalResults (SQLite) — both already built + shadow-validated for
  #741.  Only the BOUNDARY + KEY are new.
- Overlaps nix's flake eval-cache for flake refs, but covers `--expr`/
  non-flake evals and the v3 `--no-eval-cache` path it doesn't.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
