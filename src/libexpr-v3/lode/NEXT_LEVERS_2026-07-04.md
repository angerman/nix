# Next levers after the env-capture KILL — REFRAMED by workload facts (2026-07-04)

Post-Gate-C direction. Three workload facts the user stated (2026-07-04) reshape
what "beat the tree-walker" means:

1. **Eval performance ONLY** — compilation (parse+lower+emit) does not count.
2. **The same Nix expressions are evaluated MANY, MANY times** — compile once,
   amortize it to zero; the metric is *repeated-eval* cost.
3. **RSS is important** — both eval-CPU AND resident memory decide the verdict.

## The env-capture KILL is ROBUST under this reframe (no re-litigation)
The Gate C KILL used DETERMINISTIC counters that are ALL eval-phase, not compile:
- `insns` = bytecode dispatch executed during EVAL (compile is C++, uncounted) →
  env-capture +1.4% is an EVAL-CPU regression.
- run-phase arena bytes (closures/thunks/**envs**) are EVAL allocations →
  capture-repr net +12.2 MB is an EVAL-RSS regression (the +24 MB of Envs).
So under "eval-only + RSS-important" env-capture is *still* a clean KILL: it makes
both eval-CPU and eval-RSS worse.  Root cause unchanged: v3's captures avg
1.88–2.10 upvalues (Gate A C2) — too small to amortize a shared-Env header.

## The reframe changes the WHOLE v3-vs-TW question
Single warm eval (compile amortized): v3 firefox ≈ 2.44× TW CPU, ≈ 1.64× RSS
(2026-06-23 authoritative; darwin-4 re-confirm in progress).  v3 LOSES single-eval
on both axes, and every cheap lever to close that is measured-dead (reclaim GC,
thunk-avoidance, cell-shrink, interning, env-capture).  BUT single-eval speed is
the WRONG metric under fact #2.  The right metric is **amortized CPU over many
evals of the same expression** — and there the engines diverge categorically:

- **v3 has stable keys** (content-addressed CU + bytecode offsets + captured-env
  identity) → it CAN memoize/persist forced results and make eval #2..N ≈ 0.
- **TW cannot** — it re-parses + re-evaluates from scratch every invocation; there
  is no stable IR to key a result cache on.  (nix's flake eval-cache is the coarse,
  top-level-attr-only exception; it is TW's ONLY result cache and doesn't
  generalize to sub-expressions or non-flake evals.)

⇒ **v3's justification under repeated eval is the RESULT CACHE, not raw eval
speed.**  Amortized over "many, many" evals, v3+cache → ~0 CPU while TW → full
eval every time.  That is the moat, and it is the ONLY axis where v3 structurally
beats TW.  Raw single-eval CPU (the 2.44×) is a one-time cost per unique expr and
is explicitly deprioritized by fact #1/#2.

The binding constraint is fact #3 (RSS): v3 is already 1.64× TW resident, and a
result cache ADDS memory (rust-analyzer's salsa memory struggles, PERF_STRATEGY §6,
are the cautionary tale).  So the winning program = result cache for CPU + a
BOUNDED/shared cache + attention to the live-representation RSS gap.

## LEVER 1 (TOP): incremental / persistent result cache — the CPU moat
Extends `PERF_STRATEGY_2026-05-17.md` Stage 10 (salsa) and `LINKING_DESIGN
_2026-05-17.md` (content-addressed thunk cells).  Fact #2 already discharges the
salsa doc's §7 warm-fraction kill (the user asserts high repeated-eval), so the
spike is not "is warm-eval common" but "does a result cache pay for its RSS."

Foundation already present: ImportCache (memoizes imported FILE results,
coarse-grained; `importCacheResultCount()`), thunk in-run memoization (Evaluated
state).  The gap: persist/share forced results ACROSS eval roots + invocations at
sub-expression granularity, keyed on the stable CU+offset+captured-env.

### CEILING MEASURED (2026-07-04) — STRONG GREENLIGHT
Deterministic insns (host-independent, cache-off, firefox.drvPath), the spike's
disciplined first step (measure before the big build):
- firefox once (shared `p`):              36,864,730 insns
- firefox 2× via shared `p`:              36,864,738  (+8 → the 2nd is FREE; thunk
                                            memoization already shares within a root)
- firefox 2× via SEPARATE imports:        72,999,412  (≈2× → ZERO reuse across
                                            eval-roots / invocations)
⇒ An applied-import result cache converts the 2×-separate case (73 M, what happens
today across roots/invocations) into the shared case (37 M): eval #2…N → ~0.  Over
a workload that evals N derivations from the same nixpkgs, this collapses N× the
shared-infrastructure (stdenv/lib/…) re-evaluation to 1×.  **This is TW-impossible**
(no stable keys) and is precisely the moat.
RSS profile (favorable): for repeated eval of the same/overlapping exprs the cache
holds ONE forced graph (≈ what a single eval allocates anyway) and reuses it →
steady-state RSS ≈ baseline, inside the ≤1.3× SHIP gate.  Growth only across
DISTINCT (import,args) pairs → bound with the ImportCache LRU already present.

### BUILD DESIGN (the applied-import result cache)
Memoize `(import f) args → forced result`, keyed on (f content-hash [ImportCache
already has mtime/size], args content-hash).  Restrict to SOUND cases: callee is a
cached-import top-level function (pure by construction — nixpkgs is `args: <pure
attrset>`) + args is a content-addressable attrset.  Hook at OP_CALL when the
callee closure originates from a cached ImportCache function (same closure pointer
returned for repeated `import <nixpkgs>`); GC-root the cached results (ImportCache
roots are already walked); reuse the ImportCache LRU for bounding.  Two variants:
- **In-memory (spike first)**: within a process / daemon.  Measures the CPU win +
  RSS cost of retaining forced results WITHOUT the hard serialization.  Kill/ship
  here before investing in persistence.
- **Persistent (if in-memory ships)**: serialize forced Values across invocations
  (LINKING_DESIGN content-addressed cells) — the big program.
⚠ CORRECTNESS-CRITICAL (unsound memoization = silently wrong evals — worse than a
crash).  Build with fresh focus + the full byte-id ladder + brute both settings;
do NOT rush (the env-capture lesson).

### PRE-COMMITTED thresholds (write BEFORE measuring — binding rule)
Spike: build a minimal content-addressed result cache for the hottest memoizable
class (e.g. deep-forced derivation-arg attrsets / imported-module results), measure
a REPEATED-eval scenario (same drvPath eval'd twice+, fresh process, warm bytecode
cache), CPU + peak RSS + cache footprint, darwin-4, cache-off-for-compile.
- **SHIP (commit the stage):** eval #2 CPU ≤ **0.30×** eval #1 (≥70% eval-CPU
  eliminated on re-eval) AND cache-warm steady-state RSS ≤ **1.3×** the single-eval
  baseline RSS.  (Beats TW decisively on repeated eval within the RSS budget.)
- **KILL:** eval #2 CPU > **0.60×** eval #1 (lookup/deserialize/invalidation
  overhead eats the win — the rust-analyzer failure mode) OR steady-state RSS >
  **2×** baseline (cache RSS unacceptable given fact #3).
- Correctness gate: cached vs cold eval byte-identical drvPath (the whole point is
  purity → determinism); invalidation must be sound (content-address keys).

## LEVER 2: live-representation RSS reduction — the wall (foundational)
Fact #3 makes this co-primary.  v3's live representation is 1.64× TW resident and
ALL cheap reclaim levers are dead (2026-05→06 sweep) — the gap is the live objects
themselves (thunk count/size, VM structs, Boehm overhead) vs TW's 16 B niche-tagged
Value.  This is a broad foundational program, not a single lever; a result cache
makes it WORSE (more retained).  No spike proposed yet — gather here after LEVER 1
tells us the cache's RSS cost, since the two interact (a shared content-addressed
cache could REDUCE per-eval re-materialization, partially offsetting its own cost).

## LEVER 3 (measure-only): parallel eval — wall-clock, not CPU
`PARALLEL_EVAL_CAPABILITIES_2026-05-18.md` Stage 13.  No force-DAG-width tracer
exists yet (would need building — that IS the spike).  Deprioritized vs LEVER 1:
parallelism helps wall-clock at a CPU+RSS cost, and fact #3 (RSS) plus fact #2
(repeated eval, where a cache beats parallel re-computation) both argue the cache
comes first.  Revisit if LEVER 1 kills.

## Immediate
- darwin-4 warm firefox v3-vs-TW row (in progress) → the honest current gap + the
  LEVER-1 baseline; git-note it.
- env-capture path: KILLed + default-off (byte-identical), branch point 8eebbe25b.
  Retire (delete) OR keep as a validated reference/JIT-register-alloc foundation —
  a deliberate follow-up, low urgency (gated/harmless).  Its OP_MAKE/GET_ENV +
  defEnv infra is exactly what LEVER-1 key-derivation (captured-env identity) and a
  future JIT would reuse, which argues for keeping it gated a while longer.

## Part A — real-workload measurements (darwin-4, 2026-07-04, sonnet agent run)
Binary = this session's rsynced build (agent's "commit 3ff650587" reading is the
documented darwin-4 source-checkout-lags-binary caveat; insns match current HEAD
to ±3). Raw logs: scratchpad/partA-*.log.

| workload | insns 1× | insns 2×-shared Δ | warm v3 user/RSS | warm TW user/RSS | eval-CPU ratio | RSS ratio |
|---|---|---|---|---|---|---|
| firefox | 36,864,727 | +6 | 1.73s / 591MB | 0.73s / 358MB | **2.37×** | 1.65× |
| HNE | 49,454,133 | −12 | 2.64s / 1182MB | 1.55s / 563MB | **1.70×** | 2.10× |
| M5 | 225,157,973 | −436 | 6.27s / 2245MB | 3.59s / 982MB | **1.75×** | 2.29× |
| simplex | 25,370,107 | −297 | 2.62s / 560MB | 2.07s / 339MB | **1.27×** | 1.65× |

KEY READINGS:
- **Eval-only (user-time) warm gap on the haskell.nix workloads is 1.27–1.75×** —
  much closer than firefox's 2.37×; simplex is store-I/O dominated (real ≈ 32s
  BOTH engines; eval CPU is the only differentiator and it's 1.27×).
- **2×-shared is FREE at every scale** (Δinsns ≤ 0.002%): in-graph thunk
  memoization already dedups; the applied-import cache's job is exactly to make
  the ACROSS-ROOT/INVOCATION case equal the shared case (73M→37M on firefox).
- firefox post-eval: eval working set 0 B, ImportCache-pinned results 13.1 MB,
  CU bytecode 27 MB — the end-of-eval RETAINED set is small; the cache-entry
  retained-size measurement must capture the graph AT result time (a cache
  ROOTS it; post-teardown measurement shows it freed).  Big-workload live-MB
  numbers still pending (needs the current instrumentation flags re-run).
- IFD/store portions (M5/simplex) are store-bound — not eval-cache-addressable
  (already store-cached); the eval-CPU column is the cache's addressable target.
