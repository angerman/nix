# IFD provenance cache — vetted spec (2026-07-07). Fixes a LIVE stale-HIT bug + gated perf extension.

Opus design + adversarial soundness review of LITERATURE_SWEEP #1 (the moat's persistent/
cross-process IFD extension). Grounded at HEAD, file:line cited. Copyright (c) 2026 Moritz
Angermann <moritz.angermann@iohk.io>, IOG. Apache-2.0.

## VERDICT: BUILD (soundness fix, bounded) — and it FIXES A LIVE BUG
The SHIPPED IFD import disk cache (primops.cc:7245-7312) keys on `("ifd-import" ‖ path ‖
narHash(path))` — the imported file's OWN narHash only. **It UNDER-CAPTURES: if the
imported file `readFile`s/imports ANOTHER store path, that content is NOT in the key → change
that content, same imported-file path+narHash → STALE HIT (silent wrong result).** This is a
real soundness bug in what ships today (test N1 fails on current HEAD). The fix is also the
moat extension that reaches the IFD workloads (M5/HNE) the whole-eval top-level cache couldn't.

## SCOPE (what to build)
Extend the shipped IFD import cache (NOT the applied-import cache — its desc-pointer key is
in-process-only; NOT the RESULT_STORE lazy-graph serializer — out of scope). Fragment = the
IFD import (`import`/`readFile`/`readFileType`/`hashFile`/`readDir`/`pathExists` of a store
path). Reuse `disk_cache::{lookup,insert}EvalResult` (disk_cache.hh:118) + `value_serialize::
{serialize,deserialize}` (value_serialize.hh:63,69) VERBATIM.

## KEY (v2, Bazel build-then-key)
`SHA256("ifd-import-v2"\0 ‖ codegenGateFingerprint()\0 ‖ currentSystem\0 ‖ path\0 ‖
narHash(path)\0 ‖ SORTED_JOIN(provenance.contentIds)\0 ‖ SORTED_JOIN(provenance.rejectAxes))`.
The NEW part = the transitive content-id set, obtained DURING eval + folded AFTER production.
Content-ids: readFile/store→`ffi::storePathNarHash` (ffi.cc:253); IFD output→same narHash on
the realised path (post-`realisePath`, build-then-key); fetch→fetch narHash (ffi.cc:352);
getFlake→lockFileStr (ffi.cc:868); storePath→narHash; NIX_PATH→resolveNixPathContentIds (A3).

## THE CRUX — monadic input capture (soundness), reusing the A1 taint machinery
Add a thread_local `std::vector<ProvenanceFrame>` alongside `g_topLevelTaintMask`
(primops.cc:6818). `ProvenanceFrame{ axisMask, vector<string> contentIds, bool poisoned }`.
- PUSH at the fragment boundary (primops.cc:~7261, before realisePath/parse/run).
- POPULATE: every taint-bumping primop ALSO calls `provNoteRead(axis, contentIdOrEmpty)` →
  append the content-id, or set `poisoned=true` if the read has no computable content-id (a
  MUTABLE non-store path: `storePathNarHash` returns nullopt, ffi.cc:250).
- NEST: on pop, OR-merge/append the child frame into the parent (transitive IFD folds the
  inner file's narHash into the outer key). Append-only; NEVER cleared on a tryEval catch
  (a swallowed mutable read stays poisoned → over-capture is safe, under-capture impossible).
- FOLD: at insert (miss path, primops.cc:~7350), if `!poisoned` → sort contentIds, build the
  key, insert; if `poisoned` → DO NOT INSERT (fail closed).
COMPLETENESS (the enumerable obligation, A1-style): the A5-fix already routes EVERY ambient
read through a taint-bumping primop (import 7021, readFile 3148, fetch 10180+, getFlake 10461,
storePath 9933, findFile, readDir 4064, pathExists 4251); the IFD build is inside
`ffi::realisePath` (ffi.cc:172) called FROM primImport/primReadFile which bumped on entry → the
IFD build cannot happen without a bump. PRE-BUILD MUST-DO (R2): FFI-leaf audit — confirm no
leaf reads content below the primop boundary uncaptured (writeDerivation embeds no timestamp;
toStringCoerceCtx at 7141 is inside primImport's bump — assert via N4).
INVARIANT: insert iff `(result serializes) AND !poisoned AND every fired axis contributed a
content-id`. Under-capture ⇒ SerializeError-bypass or poison ⇒ no insert.

## STORAGE: reuse disk_cache EvalResults SQLite (WAL, INSERT-OR-IGNORE race-safe). v1 SAME-STORE
(narHashes are portable but a serialized string's store-context is only resolvable same-store;
currentSystem in the key ⇒ cross-platform never collides). Cross-machine sync/signing DEFERRED.
No eviction v1 (inherit shipped status quo). Byte-id: deserialize is the serialize inverse
(validated 0-mismatch #741); shadow-mode re-asserts per-workload before active reuse.

## TESTS (+/-/R, --brute core; driver run-ifd-provenance-cache-tests.sh)
- **N1 (− CRUX, FAILING-FIRST):** imported file reads store-path A (content not the file);
  RED (shipped, key=path+narHash only): change A → stale HIT; GREEN (A's narHash folded): change
  A → MISS → correct. THIS FAILS ON CURRENT HEAD (the shipped bug) and passes with the fold.
- N2 (−) IFD input-addressed rebuild → different narHash → miss-not-stale (storePathNarHash reads
  the actual NAR). N3 (−) mutable non-store readFile → poison → inserts==0. N4 (− fuzz/R2)
  tryEval-read / attrset-import __toString / 3-deep transitive IFD / getFlake-in-import → each
  keyed-or-poisoned, never keyed-with-missing-input (V3_DBG_IFD_PROV dumps the folded set+poison).
  N5 (−) tryEval swallow of a mutable read still poisons. P1 (+) pure IFD fragment → cross-process
  byte-id ACTIVE HIT. P2 (−) v1 entry not served by v2 (version partition). R1 A1→A2→A1 miss/miss/
  HIT. SHADOW mode (mismatch==0 over ≥20-fragment corpus incl. HNE callCabalProjectToNix + an M5
  IFD) before active reuse.

## SHIP GATE (pre-committed, darwin-4, git-noted)
Correctness (blocking, anywhere): --brute 22/22 + byte-id ladder + N1 red→green + shadow
mismatch==0. Cross-proc byte-id HIT on M5.name + HNE.drvPath. PERF (honest go/no-go):
`T_hit/T_eval ≤ 0.50` on HNE.drvPath, T_hit HONESTLY including the provenance-accumulator +
serialize/deserialize + per-input storePathNarHash cost. **KILL if > 0.50** (IFD_S4_FALSIFIED:
drvHashCacheDisk was +2.6% wall on M5 — lookup+deserialize exceeded re-eval). 0.50 (not 0.20)
because the IFD BUILD dominates + is amortized to the substituter across N≥3 evals.

## BUILD PLAN (2-phase — soundness first, perf gated)
1. **SHADOW/soundness (zero perf risk, fixes the shipped bug):** provenance accumulator + fold +
   v2 key + tests + shadow validation. Fixes N1 (a live stale-HIT). Ship this regardless.
2. **ACTIVE reuse (perf-gated):** flip to active only if the §gate T_hit/T_eval ≤ 0.50 clears on
   darwin-4; else the sound cache stays as the foundation, perf DEFER'd WITH DATA (A5/A1 pattern).

## RANKED RESIDUAL RISKS
R1 transitive-under-capture (HIGH→resolved: both endpoints of a pure A→B transform are folded;
the transform lives in the folded imported file). R2 FFI-leaf below-boundary read (HIGH: manual
audit + --pure-eval bound + N4). R3 readDir/pathExists non-content value (MED: store-dir narHash
sound; mutable → poison; N3). R4 IFD non-reproducibility (MED→resolved: narHash reads actual NAR;
N2). R5 tryEval swallow (MED→resolved: append-only provenance; N5). R6 unfolded string-context
(MED: same-store v1 resolvable; shadow catches divergence). R7 PERF net-negative on M5 (HIGH,
perf-not-soundness: the §gate + DEFER trigger). R8 accumulator hot-path cost (MED: only appends a
narHash when inside an IFD frame; non-IFD imports = one null-check; measure in gate). R9 cross-
version poison (LOW→resolved: v2 tag partitions).
