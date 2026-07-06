# Beat-TW: full lever + architecture review with fresh profiles (2026-07-06, HEAD de6ceb4d5)

A consolidated review — v3 VM + all prior literature — of every lever to beat the
tree-walker (TW), grounded in FRESH profiling on darwin-4. Four independent
deep-review agents (CPU levers, architecture alternatives, TW structural
advantage, repeated-eval moat) + a fresh profile-at-scale run. All four converge.

## TL;DR verdict
1. **Single-eval CPU parity is unreachable by any known lever.** The cheap-lever
   search is COMPLETE (this review re-confirmed it at HEAD). Even at ZERO
   dispatch, v3 is ~1.5-2.2× TW ("Reason B": the per-op 8B-NaN-box codec + the
   moving-GC write-barrier/nursery/safepoint tax that Boehm-TW never pays ≈ 2×
   TW's total per-op work, vs a 15-year-tuned target). A JIT is the only CPU
   step-change and it only NARROWS to ~1.5-2.2×; it does not cross 1× alone.
2. **Single-eval RSS is a defended structural floor ~1.6-2.0×** (P2 falsifier
   KILL, 8e64923e2: the ~837MB dead M5 arena is uniformly interleaved, 0MB
   reclaimable-to-OS). Representation-shrinking banked ~74MB (P1a+P1b) = ~2-3%.
3. **The WINNING play is the repeated-eval MOAT.** v3 beats TW at **N≥3** evals of
   the same expression (amortized `k·T_TW/N < T_TW` ⇔ N>k, k≤2.5) — and that
   regime (CI, `nix build` loops, daemon, dev iteration, Hydra) is the NORM, not
   the exception. TW structurally cannot cache across runs. This is the only axis
   where v3 is structurally AHEAD.

## FRESH PROFILE (darwin-4, cache-off, de6ceb4d5 — the "where do we spend time")

### Deterministic dynamics (host-independent, reliable)
| workload | ops | thunks alloc | % never forced | nursery hit |
|---|---|---|---|---|
| firefox | 36.8M | 2.83M | 66.0% | 22.8% |
| M5 (cardano) | 224.5M | 16.1M | 65.8% | **7.7%** |
| HNE | 49.3M | 4.03M | 67.8% | 44.5% |

### Opcode histogram (stable across all 3; firefox shown)
`GET_UPVALUE 14.6% · SET_LOCAL 11.9% · GET_LOCAL 10.5% · MAKE_THUNK 8.3% ·
GET_LOCAL2 7.0% · RETURN 7.0% · BRANCH_FALSE 4.6% · ATTRS_SELECT 2.5% · …`
→ **~46% of all opcodes are trivial local/upvalue load-store shuffling**, each
paying full dispatch. Classic dispatch-bound signature.

### On-CPU self-time (fresh `sample`, excl. kernel-wait)
- **firefox**: `dispatchLoop` 226 (plurality; the trivial ops are handled INLINE
  here) ≫ `mergeBindings` 66 > TLS `_tlv_get_addr` 45 > `countDistinct` 24 >
  `forceValue` 20 > string ops.
- **M5**: `dispatchLoop` 1733 ≫ `forceValue` 175 > **GC** (`tryMark` 115 +
  `visitValue` 48 + `fwdThunk` 31 + `Arena::alloc` 30 ≈ 224, ~10-12%) >
  `lookupStringContextEntries` 56 > `deserializeCU` 53 (disk-cache load) >
  `allocThunkSuspended` 50 > `callClosure` 38 > `applyForceWriteback` 35 > regex
  57 > `PosSnapshotKey` hash 29 > `realizeMapAttrsEntry` 21 > `globalInternSymbol` 21.

**Reading**: dispatch (incl. inline trivial-op handlers) is the plurality;
separate-Thunk allocation (68.8% never forced) is the #2 structural cost; GC is
~10-12% at M5 scale; then a diffuse tail (merge, TLS, string-context, CU-
deserialize, regex, symbol/pos interning). No single dominant fixable hotspot —
the cost is spread across per-op semantic work.

## CPU LEVER TABLE (shipped / killed / remaining)
| lever | on-CPU % | effort | prior verdict | recommendation |
|---|---|---|---|---|
| **JIT** (native hot bodies, register-VM operand model) | dispatch+per-op → ~1.5-2.2× | multi-week (needs J3 safepoints) | RCA'd/deferred; narrows, ≠ beat | THE only CPU step-change; multi-quarter; won't cross 1× alone |
| register-VM standalone | ~5-10% (K≥16) | multi-week | measured 2026-06-29 | not standalone → only INSIDE the JIT |
| countDistinct memoize | ~9-15% | — | **SHIPPED** (L1) | closed |
| SET_LOCAL_KEEP superinstruction | +1.4% wall | — | **SHIPPED** | closed; 2nd superinstr = register-VM territory |
| OP_GET_UPVALUE inline cache | ~2-4% | medium | UNBUILT | marginal; real fix = JIT reg-alloc |
| computed-goto dispatch | ~0 | small | **NEUTRAL (CG-1..4)** | **dead — do not revisit** (cost is per-op WORK not switch) |
| thunk-churn / strictness | ALLOC slice | large | **KILLED** (T2a: 0.85% removable; 99.3% genuine laziness) | dead as strictness; only a JIT (inline bodies) removes it |
| TLS hoisting | was ~6% sample | small | **FALSIFIED+reverted** (T1a) | dead (OOO-hidden, not attackable) |
| FFI/primop marshalling | <0.5% | — | not a target (hot primops v3-native) | dead (also V3-NATIVE rule) |
| forceValue/callClosure | ≤3% | — | overturned narrative | dead |

**Conclusion: the cheap-lever search is complete. v3 is not doing a single
fixable thing wrong on CPU.**

## ARCHITECTURE VERDICT
| architecture | CPU vs TW (single-eval) | status | recommendation |
|---|---|---|---|
| **Incremental / result cache (the moat)** | **≪1× repeated; 1× cold** | applied cache SHIPPED default-on (eval#2 free, byte-id); top-level phase-1 active default-off | **PURSUE — the only game-changer** |
| Copy-patch→optimizing JIT | ~1.5-2.2× (→1.3-1.8× w/ reg-alloc) | J0-J3 proven standalone; integration unbuilt; J3 GC-safepoint blocker | CPU-quality investment; won't beat TW alone |
| Parallel / multi-core (Stage 13) | <1× wall on parallel-structured only | speculative, no trace run | measure critical-path first; process-parallel + async-I/O first |
| AOT cached bytecode / LINKING | ~1× (attacks cold parse ~5% warm) | AOT shipped opt-in | foundational substrate for the cache; not a beat lever |
| threaded-code / superinstructions | ~1× (dispatch ~5% wall) | computed-goto NEUTRAL; SET_LOCAL_KEEP shipped | harvest cheap peepholes only (largely done) |
| 16B Value + inline thunk | ~1× CPU (RSS lever, +1-4% wall) | cell-shrinks shipped; inline thunk BLOCKED by moving GC | do not fund on RSS grounds (P2 KILL → 1.6-2.0× floor) |
| HAMT persistent attrsets | **negative (1.3× CPU / 4.7-9.2× RSS)** | **KILLED + deleted** | **dead — do not revisit** |

## TWO GENUINELY NEW, ACTIONABLE FINDINGS this review surfaced
### N1 — The non-moving-GC CPU case is UNMEASURED (the biggest unexplored lever)
The P2 GC-rewrite falsifier KILL (8e64923e2) was **RSS-only** (reclaim-to-OS at
peak = 0MB). It did NOT evaluate the **CPU** case for a non-moving GC. But TW's
#1 per-op advantage (inline `{Env*,Expr*}` thunk = ZERO separate allocation) is
blocked in v3 SOLELY by the *moving* GC (the PhD-6 root cause is the moving GC,
not laziness): a Value cell inside a tenured Bindings can't hold a mutable
forwardable payload, so v3 indirects through a separately-allocated 24B Thunk
cell — which the profile shows is the #2 CPU cost (68.8% never-forced, ALLOC
~20%, `allocThunkSuspended` on the M5 leaf list) AND drives the moving-GC tax
(`tryMark`/`fwdThunk`/`visitValue`/barriers ≈ 10-12% on M5). A **non-moving
tenured region (Immix/mark-region)** would (a) unblock TW-style inline thunks
→ eliminate the separate-cell alloc, (b) drop the Phase-D write-barrier +
scavenge bookkeeping. This CPU justification is DISTINCT from the RSS
justification that was killed, and it has never been measured. **Recommended
spike**: a "16B direct-tag Value + non-moving collector + inline thunk"
prototype, measured for CPU (not RSS) — potentially the largest single lever on
"Reason B." Falsifier: does eliminating the separate-thunk-cell alloc + barriers
buy ≥15% warm CPU on firefox/M5? (Independently flagged by both the
architecture-alternatives and TW-structural reviews.)

#### C1 SOUND MEASUREMENT DESIGN (2026-07-06) — decompose the "Reason B" tax into 3 additive components
Gate: total tax (barrier + separate-thunk-alloc + scavenge) **≥15% warm CPU on
BOTH firefox AND M5 → GO** (fund the non-moving-repr prototype); **<8% → KILL**.
The non-moving inline-thunk repr eliminates ALL THREE, so their SUM is the ceiling.

**Do NOT** naively no-op the write-barriers with scavenge live: the barriers
build the generational remembered set (`dirtyContainers`, barrier.hh:238/258/276/287
+ thunkSetEvaluated:287) that young-gen scavenge consumes as old→young roots.
Scavenge is hardcoded-ON (CLAUDE.md constraint 0) → a barrier no-op with scavenge
live = missed-root UAF = NOT byte-identical. The three components must be
measured by three DIFFERENT sound methods:

1. **Barrier instruction cost (C1a) — byte-id A/B in a NO-SCAVENGE config.**
   The per-write barrier cost is a FIXED per-instruction cost, workload-independent.
   Measure it byte-id on firefox with a nursery large enough that NO scavenge fires
   for the whole eval (`NIX_V3_NURSERY_SIZE` ≫ firefox young-gen; the remembered set
   is then never consumed → no-op'ing the barrier writes is byte-identical BY
   CONSTRUCTION). Build a `#ifdef NIX_V3_BARRIER_NOOP` (or constexpr) variant that
   compiles the dirtyContainers pushes to nothing; A/B firefox CPU (median-of-5,
   darwin-4). Δ = pure barrier %. NB firefox big-nursery avoids scavenge; M5 CANNOT
   (GBs of churn) → for M5, scale: barrier% = (M5 barrier-invocation-count ×
   per-write cost) / M5 CPU, with the count from a barrier counter.
2. **Separate-thunk-alloc cost (C1b) — on-CPU sample + counter.** `allocThunkSuspended`
   self-time from `bench/profile-at-scale.sh` sample (firefox + M5) — the alloc+init
   of the 24B Thunk cell an inline {env,code} repr removes. Cross-check vs thunk
   alloc count × per-alloc ns.
3. **Scavenge cost (C1c) — on-CPU sample.** The Cheney young-gen copy self-time
   (tryMark/fwdThunk/visitValue) from the same sample. Prior profiles: ~1.2–7% M5.

Sum C1a+C1b+C1c on firefox AND M5 → render GO/KILL. Prior rough estimate lands in
the ambiguous 8–15% band, so the median-of-5 precision on darwin-4 matters. darwin-4
is REACHABLE (verified 2026-07-06, 8-core arm64). This is ~1–2 focused darwin-4
build+measure cycles (barrier-noop build + firefox big-nursery A/B + counters + M5
sample); it is NOT a same-turn task and touches the GC-UAF-risk surface, so it
warrants its own session with the byte-id no-scavenge invariant verified first.

#### C1 VERDICT (2026-07-06) — MEASURED on darwin-4 (median-5 A/B) → **GO** (barrier tax alone ≥15%)
Gate: ≥15% warm CPU on BOTH firefox AND M5 → GO; <8% → KILL; 8–15% marginal.
Method: `-DNIX_V3_BARRIER_NOOP` A/B (phaseDActive()→false ⇒ all 11 Phase-D barrier
blocks dead-code-eliminated), both builds run identically, warm cache, median-of-5.

| workload | A (barriers on) | B (barriers off) | **C1a barrier tax** | byte-id |
|----------|----------------:|-----------------:|--------------------:|:-------:|
| firefox.drvPath | 2.01 s | 1.68 s | **16.4 %** | Y (5/5) |
| git.drvPath     | 0.84 s | 0.68 s | **19.0 %** | Y (5/5) |
| M5 (cardano-node.name) | — | ABORT | not directly measurable | — |

**VERDICT = GO.** The write-barrier removal ALONE buys 16.4 % (firefox) / 19.0 %
(git) warm CPU — both clear the ≥15 % GO threshold with margin, on two
independent workloads. This is a **LOWER BOUND** on the non-moving inline-thunk
ceiling: B still separately-allocates every 24B Thunk cell (C1b ≈ 2 %,
`allocThunkSuspended` sample) and still runs the moving scavenge-copy (part of the
GC 8.2 % bucket), BOTH of which a non-moving repr additionally eliminates. So the
true ceiling is ~18–24 % on firefox. Much of C1a is the barrier's `threadNursery()`
TLS load per write (`_tlv_get_addr` ≈ 10 % of sampled self-time) + `isNurseryPayload`
+ `dirtyContainers().push_back()` — all gone in a non-moving repr.

**This PARTIALLY OVERTURNS the "single-eval CPU is JIT-only" leaning:** the moving
GC is a ≥16 % single-eval CPU lever, distinct from dispatch (JIT territory). Per the
goal, GO = *recommend funding* the non-moving-repr prototype — NOT build it now
(the full GC rewrite stays out of scope; STOP at the verdict for the human).

**Soundness caveats (honest):**
- No truly scavenge-free config exists: `gen-major` (g_genMajorEnabled hardcoded,
  vm.cc:381) forces a nursery scavenge at each exitDepth==0 safepoint regardless of
  `NIX_V3_NURSERY_SIZE`. firefox/git got 2/1 forced scavenges in BOTH A and B; B was
  byte-identical anyway (verified 5/5 runs → those scavenges had no barrier-dependent
  roots). Tight variance (A 2.01–2.04, B 1.67–1.69) ⇒ real signal, not noise.
- M5 is NOT directly measurable: under B its forced scavenge hits a barrier-recorded
  old→young root → `STALE THUNK ... nursery=YES` abort (this IS the UAF-class negative
  the gate asks for — barriers are load-bearing). A GB-scale eval can never run
  scavenge-free (nursery can't hold GBs of young-gen churn), so no byte-id A/B is
  possible for M5. M5's barrier tax is EXTRAPOLATED ≥ firefox's 16.4 %: M5 is
  markedly MORE thunk-heavy (profiles: 637 MB thunks dominant) → higher inter-gen
  write density → ≥ firefox barrier %. The M5 leg is therefore an estimate, not a
  direct number; firefox+git are the direct GO evidence.
- Local +/−/R (laptop, byte-id runs anywhere): + huge-nursery noop == baseline
  drvPath; − tiny-1MB-nursery noop → SIGSEGV (scavenge fires, barriers missing);
  normal build (barriers on) at 1MB nursery → no crash, byte-id (the edit is inert
  without the flag). Guarded as reproducible via `-DNIX_V3_BARRIER_NOOP`.

### N2 — Content-addressed IR fragments make eval#1 faster cross-file/cross-machine
Today the CU disk cache is FILE-granular (whole-file source SHA). Two files
sharing `lib.fix`/`mkDerivation`/`mapAttrs`-shape lambdas re-lower
independently. Structural hash-consing of IR fragments (recursive over free-var
hashes, salted by the opcode fingerprint — Unison Item 1 / ABT Item 2) makes
"the second time anyone anywhere evaluates anything using `lib.fix`, it compiles
once globally." This attacks the **~40% compile share** (parse+lower ≈ 4.5s of
M5's cold ~11s) on the COLD/first eval across files and machines — the CI regime.
~3-4 weeks (ABT refactor + content-addressed IR, ~600 LOC). Reinforces the moat.

#### B1 VERDICT (2026-07-06) — MEASURED, gate = ≥2× cross-file dedup → **DOCUMENT-CLOSE (KILL)**
The pre-committed gate for building B2 (content-addressed IR fragments / ABT):
cross-file IR-fragment dedup **≥2× → GO**, **<2× → document-close**.

**Measured 1.08–1.32× — a huge margin below 2× → B2 does NOT clear the gate.**

Method: `NIX_V3_DEDUP_SURVEY=1` (FNV-1a per-lambda bytecode fragments, process-wide
`seenHashes`), full drvPath evals on nixpkgs 24.05 (zw3rk checkout, aarch64-darwin)
via `v3-eval` — the survey observes ALL compiled AND disk-loaded CUs (see the
instrument fix below):

| workload         | totalFns | uniqueFns | **fn_dedup_lb** | totalBytes | **byte_dedup_lb** |
|------------------|---------:|----------:|----------------:|-----------:|------------------:|
| hello.drvPath    |   91 427 |    69 451 |         1.32×   |  5 367 KB  |       **1.08×**   |
| git.drvPath      |   92 387 |    70 074 |         1.32×   |  5 425 KB  |       **1.08×**   |
| python3.drvPath  |   91 399 |    69 431 |         1.32×   |  5 366 KB  |       **1.08×**   |

- **byte_dedup_lb = 1.08× is the honest lever** (compile/storage work saved).
  fn_dedup_lb = 1.32× (count) is inflated by ~22 K tiny identical stubs
  (`x: x`-class) whose bytecode is a handful of words — deduping them saves only
  **~8% of bytecode bytes**, nowhere near the 2× that would fund an ABT rewrite.
- **Corroborates the prior Stage 9 LINKING KILL** (2026-05-22, thunk-body IR-level
  dedup = **1.17×**, killed at the same 2× threshold). Two independent
  measurements (IR-level 1.17×, bytecode-level 1.08–1.32×) agree: cross-file
  structural redundancy is LOW because the FILE-granular CU disk cache + the
  applied-import cache already capture the whole-file sharing; sub-file fragments
  that recur are small.
- Note the survey's count ratio is a LOWER BOUND on true alpha-equivalent dedup
  (run.cc:929) — but the prior IR-level 1.17× already measured the
  alpha-equivalent regime and lands in the same band, so reaching 2× is
  falsified from two directions.

**Disposition:** B2 (ABT + content-addressed IR fragments) is NOT built — the
eval#1/cold-CI compile lever is real but ~8%, disproportionate to a 3-4 wk ~600
LOC rewrite. The moat (Phase A: applied-import cache SHIPPED + top-level
cross-process cache) remains the strategic direction. A CLEAN KILL = deliverable.

**Instrument fix shipped with this verdict** (the survey was under-observing by
~700×): (1) warm imports HIT the CU disk cache and restore via `deserializeCU`
(primops.cc:7514), bypassing the fresh-compile observe (8164) → added a
`surveyCUBytecodeDedup` at the disk-load path (7523). (2) The per-runRootExpr
report (run.cc:933) is only reached by the builtins-install evals (the user
eval's imports run re-entrantly via `run()`, not `runRootExpr`) → it printed a
mid-eval snapshot of ~131 fns; added an `atexit` reporter (`dedup_survey [FINAL]:`)
reading the same process-wide accumulator after all imports are surveyed.
Guarded by `test/run-dedup-survey-tests.sh` (+cold/+warm/-off, in --brute core).

## THE MOAT — quantified (the recommended strategic direction)
- **Applied-import cache (in-process): SHIPPED default-on** (eb9653706). Measured
  eval#2 = **0.00× CPU** on hello/firefox (in-graph thunk memo makes it free);
  firefox 2×-separate-import 73M→37M insns. Byte-identical, --brute clean.
  CAVEAT: does ~0 for FLAKE workloads (M5/HNE: **99.76% of overlay args are
  unhashable functions**) — only pure-nixpkgs (firefox-class) benefits in-process.
- **Top-level cross-process cache: built, phase-1 ACTIVE, default-off** (0868e81ec).
  A hit skips the ENTIRE pipeline (parse+lower+run). Falsifier `T_hit/T_eval =
  0.044`. This is the tier that generalizes to ALL workloads (caches the whole
  result, not the args). **Blocker = precise data-flow impurity taint**: nixpkgs
  calls `currentTime`/`getEnv` in result-IRRELEVANT branches, so conservative
  global taint over-rejects the very workloads it targets. THE crux.
- **Persistent forced-result store: KILLed** (`T_hit/T_eval=1.00` — reload+relink
  of a forced graph ≈ re-eval; the cost is force, which a store can't avoid).
- v3 beats TW at **N≥3** evals; crossover regime = CI/daemon/loops = the norm.

### Roadmap (priority order)
1. **Precise data-flow taint** for the top-level cross-process cache (or the
   empirical-corpus shortcut). Turns the biggest TW-impossible win (skip the
   whole cold pipeline) from default-off to production. Covers all workloads.
2. **Soak + default-on-audit** the applied cache (shipped; complete the nixpkgs
   byte-eq sweep). Banked; keep healthy.
3. **Resolved-NIX_PATH content key + codegen fingerprint** → cross-machine
   soundness (small).
4. **Content-addressed IR fragments** (N2) — the eval#1 / cold-CI lever.
5. **Optional CPU-quality track**: the N1 non-moving-GC-for-CPU spike; then the
   JIT (only as a deliberate multi-quarter program, knowing it narrows not beats).
6. **Do NOT**: rebuild the persistent forced-result store (KILLed), chase RSS
   parity (structural floor), or revisit HAMT / computed-goto / TLS-hoist / thunk-
   strictness (all KILLed).

## Honest framing for the human
"Beat TW" has two readings. On **single-eval speed/memory**, v3 cannot win — it's
a measured wall (15-year-tuned interpreter; v3's per-op tax is ~2× even dispatch-
free; RSS floored at 1.6-2.0×). On **repeated eval** (the real production
regime), v3 already wins at N≥3 and TW structurally cannot follow. The strategic
call is to STOP chasing single-eval parity and FUND the moat (precise-taint
cross-process cache + content-addressed IR), with the non-moving-GC-for-CPU spike
(N1) as the one unmeasured single-eval lever worth a falsifier before the JIT.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
