# Literature sweep: GHC / Unison / Tvix / Lix / Perceus / incremental-computation — did v3 miss an applicable idea? (2026-07-07)

Five parallel opus research agents (GHC-STG runtime; Perceus/reuse-RC; Tvix+Lix+cppnix
prior art; incremental-computation/memoization; Unison runtime+effects) + a parallel-
potential trace, each grounded against v3's shipped/killed prior art so findings are
GENUINELY-MISSED ideas, not re-surfaced known ones. Reconciled adversarially. Copyright
(c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, IOG. Apache-2.0.

## META-CONCLUSION (the reassuring negative + the one real opening)
**No evaluator or paper found a Value/thunk representation that escapes v3's walls.** The
two closest peers CONFIRM the walls are structural, not our mistake:
- **Tvix** (from-scratch Nix bytecode VM, Rust): 16B tagged enum, `Rc<RefCell>` thunks,
  **no GC** → leaks cycles (their #1 regret; they want mmtk = a move TOWARD v3's design),
  clones Values on force, **no result cache**. "10× faster" is micro-bench theater — **no
  published nixpkgs-scale win over cppnix by anyone**.
- **Lix**: shrank Value to a 1-word refcounted pointer → **−15% mem but +3% eval-CPU
  REGRESSION**; their real lever is **parallelism** (Determinate's atomic-thunk work).
- **cppnix** shrank Value 24→16B in 2.30 (~20% eval-mem) — **the TW baseline is getting
  leaner**; RSS claims must name the cppnix version.
- Everyone converges on the SAME small levers: shrink Value, intern symbols/strings, go
  parallel. **v3's moat (applied-import result cache) is AHEAD of upstream** — cppnix
  #6228 explicitly WANTS a general persistent function-eval cache = what the moat is.

The one real opening the sweep surfaced is not a single-eval-parity lever (those stay
structurally out of reach) — it is an **extension of the moat** (the actual win) that
covers the IFD workloads the whole-eval cache could not. See #1 below.

## RANKED SHORTLIST — genuinely-untried + applicable (value × cost, with my cross-checks)

### 1. [HIGHEST] Fragment-level constructive-trace cache with IFD-boundary content-hash folding
The build-systems-à-la-carte lens: Nix EVAL = **suspending-scheduler × constructive-traces-
rebuilder** (the Rock/"Cloud Shake" quadrant), DISTINCT from Nix-the-build-system (deep-
constructive-traces). The moat IS "constructive traces" — v3 is on the right model.
**The killer move (Bazel Remote-Execution ordering): you do NOT need IFD outputs in a
PRE-eval key.** Split the eval at the IFD boundary; build the derivation; take its
content-addressed output store-path hash; fold THAT into the downstream fragment's key
AFTER it is produced: `key = (evalKey, [inputHashes…, ifdOutputHash]) → resultValue`. Nix's
`ca-derivations` supplies the CA digest → determinism precondition met (no Frankenbuild).
**This DISSOLVES the IFD-in-the-key wall that made me conclude the whole-eval top-level
cache couldn't serve M5/HNE.** NOT hypothetical: haskell.nix `materialized/` does exactly
this BY HAND (key = plan-sha256 over inputs; hit on 2nd eval; degrade to build-then-cache
on miss); the IFD_DEEP_DIVE_2026-05-21 §S4 / UNISON_IDEAS item-3 design AUTOMATES it
(designed, NOT landed).
- **My cross-check / honest catch:** the IFD *ordering* is solved, but capturing the
  COMPLETE input set of each fragment is a MONADIC (runtime-discovered) problem —
  under-capture = silent whole-eval miscompile (the exact soundness class of the A1/A5
  work). So this = the moat, extended, riding the A1 precise-taint machinery. It is real
  engineering, not a wall. This is the highest-leverage new direction because it
  strengthens the WIN (repeated-eval moat) and reaches the IFD workloads that matter.
- Value × cost: HIGH × MEDIUM. Builds on the shipped applied-import cache + the top-level
  cache mechanism (already built) + A1 taint.

### 2. [HIGH, CPU] Cheap-eagerness / optimistic evaluation — attack the 62-68%-never-forced thunk wall at its ROOT
The literature's #1 CPU lever, and genuinely distinct from v3's shape-heuristic thunk-
avoidance. Two flavors:
- **(a) Cheap-eagerness (Faxén; static, conservative):** eagerly evaluate provably-cheap
  + total RHSs (literal / var / small arith / saturated pure builtin) → no thunk alloc.
  Purely syntactic, no types. LOW-risk, partial coverage. UNTRIED (opt removes *trivial*
  var/const thunks; this covers *cheap-but-nontrivial*).
- **(b) Optimistic/speculative eval (Ennals & PJ ICFP'03; adaptive):** speculatively eval
  the RHS with chunky bounds + an abort net + per-site runtime profiling that reverts bad
  sites to lazy. GHC got 5-25% on this exact "most thunks forced-or-cheap" problem. Nix's
  purity makes abort CLEANER (a speculation hitting `throw`/`assert`/infinite-recursion
  just reverts, no observable effect). HIGH-risk (abort/profiling machinery).
- **My cross-check (decisive):** v3 measures **62-68% NEVER-forced** → blind speculation
  WASTES work on those. The win requires a **per-site BIMODAL force-rate** (some sites
  always-forced → speculate; some never → keep lazy) that the adaptation sorts.
- **MEASURED (2026-07-07, NIX_V3_FORCERATE_TRACE per-creation-site histogram; git/python3/
  hello; byte-id ON==OFF; brute 36/36):** OP_MAKE_THUNK-site never-forced = 41-44% (< the
  62-68% whole-population figure — this scopes to MAKE_THUNK sites only). Distribution is
  **extreme-weighted (67-69% of created mass at the 0%+100% extremes) but NOT clean-bimodal**
  — a fat 50-70% "genuinely-mixed" middle bump (~14-15%). GOOD: **~75% of never-forced thunks
  are at <10%-rate sites** (a classifier keeps them lazy correctly), only **~1% at ≥90% sites**
  (mis-speculation waste ≈ nil) → the "don't waste work on never-forced" precondition is MET.
  BAD (the killer): only **~28% of the always-forced mass has a statically-cheap/bounded/
  non-lazy RHS** → static cheap-eagerness can safely eager-eval only **~11% of all thunks**
  (the other ~72% of always-forced RHSs are heavy — nested calls/thunks, unsafe to eager).
- **VERDICT: NOT a priority lever.** (a) static cheap-eagerness = LOW ceiling (~11% of thunks →
  low-single-digit % CPU); (b) adaptive optimistic-eval could reach the 14-15% mixed middle +
  heavy-always-forced but at HIGH cost (abort + per-site profiling) + risk (eager heavy/
  unbounded work). Both attack the single-eval CPU axis which is a structural dead-end anyway.
  Measure-first did its job: KILLED as a priority before any build. Instrument (forcerate_trace)
  kept gated for future re-measurement. Value × cost: (a) LOW-CEILING × LOW; (b) LOW-value × HIGH.

### 3. [MED, CPU/RSS] Scavenger indirection-shortcutting (evaluatedness-tag + selector thunks)
GHC's collector opportunistically (i) shortcuts an already-forced thunk's indirection and
(ii) forces a selector thunk (`x.field`) whose target is already evaluated — **during
scavenge**. For v3: `attr.field` / list-index ARE selector thunks; deep lazy attrset/list
graphs are the hot case. The moving scavenger ALREADY walks these cells — piggyback the
shortcut: re-point a container's Value at the forced value + mark it, so the force path
skips the 24B cell-load (the "semi-tagging" branch-mispredict win, ~7-9% class in GHC) AND
live selector chains collapse (fewer thunks + lower residency).
- **My cross-check:** this MERGES the GHC agent's two ideas (evaluatedness-bit + selector-
  shortcut). The evaluatedness "bit" is NOT a free NaN-box bit (A1 recorded none exist) —
  it's realized by the SCAVENGER re-tagging container Values, exactly GHC's myth-bust
  ("the moving GC is the tag-MAINTAINER, not a blocker"). Unlike the KILLED reclaim levers
  (which fought the arena's never-munmap limit), this collapses LIVE representation — a
  different, still-open axis. MEASURE selector-chain depth on M5/firefox first to size it.
- Value × cost: MED × MED. Fits existing scavenger.

### 4. [MED, barrier] Card-marking as an alternate write-barrier representation
The single genuinely-different barrier design v3 hasn't measured: one branch-predicted
byte-store per write into a fixed card table + a GC-time card scan, vs the per-container
`dirtyContainers` push (which dedups + grows). A re-balance (adds GC-time scan), not a free
win — but at a MEASURED 16-19% barrier tax it's the one alternative barrier worth an A/B.
- Value × cost: MED × MED (a measured spike).

### 5. [LOW ceiling, CPU] is-unique thunk-reuse micro-trick (the one salvageable Perceus kernel)
When a thunk is forced + about to be memoized, if it's provably sole-owned (rc==1 / not-
yet-shared bit), reuse its 24B cell for the result or free it immediately instead of
tenuring — targeting the short-lived thunk churn. DYNAMIC (sidesteps lazy-liveness
undecidability that kills full Perceus) + LOCAL (sidesteps cycles: a cycle member is never
rc==1). **Catch:** v3 cells carry no is-unique bit (add bit + a barrier to maintain it =
reintroduces per-alias cost); and our BiBOP/mid-eval-reuse KILLs showed the arena never
munmaps + dense interleaving → freeing sole-owned thunks reclaims ~0MB to OS → only a CPU-
churn win, ceiling bounded by how often is-unique holds under Nix's heavy sharing
(probably LOW, per Perceus's own "sharing → slow path"). MEASURE-FIRST (is-unique hit-rate
at force time). Value × cost: LOW × MED.

### 6. [ORTHOGONAL, ecosystem] Parallel evaluation — MEASURED CEILING (2026-07-07)
Determinate's atomic-thunk-state work: `nix flake show` 4.1× (12c), `nix search` 3.0×
(16t); Lix is adopting it. **Parallel-potential trace (NIX_V3_PAR_TRACE work/span, byte-id
ON==OFF):** op-weighted (realistic) ceiling = **17.9× hello / 24.5× git / 17.7× python3 /
46.6× M5**; count-based 164-229×; deepest single force-chain only 129-191 vs 0.77M-11M
total forces (a WIDE, SHALLOW DAG — NOT a deep serial spine). So the DAG has ENOUGH width
to feed 8 cores — **it is NOT Amdahl-dead** (this UPDATES the doc's pessimistic 1.2-2× per-
workload table; the earlier take was too low). BUT: **~48-53% of all force-REQUESTS are
memo-hits** (shared/memoized thunks) → half the work is serialized-by-reuse and can't be
re-parallelized — exactly why Determinate measured SUB-linear (3-4× on 8-16c, "stdenv
serializes"). And the ceiling is IDEALIZED (ignores sync + GC-lock + FFI-serialization,
which §5 says cap real wins well below it). Net: parallelism CAN buy ~3-4× WALL-CLOCK
(matching the ecosystem), but it's ORTHOGONAL to the per-op/RSS walls + the moat, HARD
under a moving GC (can't move cells under concurrent readers), disproportionately expensive
(9-15mo + a parallel GC — bigger than the non-moving-GC change we KILLed), and the
1.7k-16.8k INDEPENDENT ROOT forces mean the free process-level path (nix-eval-jobs / Hydra
/ `xargs -P`) already captures the wide parallelism. Cheaper adjacent if single-eval LATENCY
is the goal: I/O concurrency (async, ~4-8wk), speculative pre-forcing (~2-4wk). Value × cost:
MED wall-clock × VERY HIGH. Not the beat-TW lever. (Trace instrument: par_trace.{hh,cc},
gated NIX_V3_PAR_TRACE, byte-id-neutral.)

### 7. [CHEAP refinements] Moat hardening
Failure-caching (cppnix `Failed` type — cache throws too); Salsa durability firewall
(nixpkgs=HIGH / user-file=LOW → O(1) skip the nixpkgs subgraph on a small edit, the edit-
driven regime); hash-queryable cache CLI (ergonomics). All cheap, all refine the moat.

## CONFIRMED DEAD (the sweep corroborated our KILLs / added model-level KILLs)
- **Reference counting / Perceus AS A GC REPLACEMENT: dead** — Nix's `rec`/`let-rec`/`fix`
  create genuine heap cycles (codebase-confirmed); the whole RC family excludes cycles BY
  CONSTRUCTION (Koka/Lean can't create them) → RC-for-Nix must add a tracing cycle-collector
  (Lins) = back where we started + RC traffic. Sharing kills the reuse win (Perceus's own
  data). RC does NOT remove the barrier (competitive deferred RC needs a log-barrier +
  stack maps; Bacon's tracing/RC duality = no free lunch). Model-level KILL; only #5 above
  survives.
- **Runtime value hash-consing (for RSS/equality): dead** — our #772 redundancy was 1.17×;
  our repeated finding is "the RSS gap is LIVE DISTINCT representation, not duplicated
  subgraphs" — the exact low-redundancy regime where hash-consing LOSES (JuliaSymbolics
  2025: +2.5× memory when redundancy is low). Closure-hashcons already measured ~5MB (D1).
- **Maximal-laziness / content-addressed IR (Dolstra LDTA'08 broad; our N2): low-value** —
  same low-redundancy data; the narrow version (IR-fragment dedup) already KILLed at
  1.08-1.32× ≪ 2×.
- **Effect/ability-row cacheability (Unison): not portable** — Nix impurity is ambient +
  eager + un-typed (no `'`/handler boundary, `with`-scope dynamic dispatch, control-flow-
  launder). Confirms WHY the taint/empirical-corpus approach (A1) is the right shape.
- **Myth-busts (do not pursue):** eval/apply (v3 is already push/enter — the paper says
  that's fine for interpreters); non-moving concurrent GC + SATB (a LATENCY technique, ~10%
  throughput COST — wrong axis, + overlaps the non-moving-tenured KILL); UNPACK / worker-
  wrapper / CPR (need static ADT layouts Nix's dynamic attrsets lack); full-laziness/float-
  out (INCREASES residency — anti-goal); Adapton full DCG + HAMT persistent attrsets
  (retained-graph RSS fights the moving GC + the floor; HAMT already KILLed 1.3×CPU/4.7-9.2×
  RSS); deep-constructive-traces (wrong rebuilder for the monadic eval graph).

## RECOMMENDATION
The sweep REINFORCES the strategic conclusion — single-eval parity is structurally out of
reach (3 peer evaluators + the literature agree) — and the WIN is the moat. The one high-
leverage NEW direction is **#1: extend the moat to a fragment-level constructive-trace
cache that folds post-build IFD content-hashes into the key (Bazel ordering) + captures
inputs precisely (A1 taint)** — it covers the IFD workloads (M5/HNE) the whole-eval cache
structurally could not, and it's a designed-but-not-landed idea (IFD_DEEP_DIVE §S4). The
CPU levers (#2 cheap-eagerness, #3 scavenger shortcutting, #4 card-marking) are real but
narrow and gated on cheap measure-first histograms; none beats TW's per-op efficiency
alone. Parallelism (#6) is the ecosystem's chosen wall-clock lever but orthogonal to our
walls and disproportionately expensive.
