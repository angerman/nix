# Performance Strategy — candidate stages beyond STG + V8 + Unison + linking

**Status: CANDIDATE STRATEGY, NOT YET COMMITTED TO ROADMAP.**

This document records the architectural analysis around two candidate axes that may eventually be added to `ROADMAP_TO_VISION_2026-05-15.md`:

- **Incremental / salsa-style eval caching** as a high-leverage attack on warm-eval workloads.
- **Persistent attrset data structures (HAMT / CHAMP)** as a Nix-specific attack on overlay-heavy `//` patterns.

It also covers the JIT question and where it sits in the priority stack.

**Critical disclosure (§6)**: a verification agent on 2026-05-17 pushed back on the load-bearing empirical claim. The proposal below is **directionally plausible but empirically unverified**. The recommended next step is a 1-week measurement spike (§7), not stage commitment.

## 1. Premise — the workload-mode hypothesis (UNVERIFIED)

The current ROADMAP optimizes for **cold-eval workloads**: first-time evaluation of a Nix expression. STG runtime, V8 PICs, and content-addressed code cache all address cost-per-eval.

**Hypothesis**: real Nix wall-time is dominated by **warm-eval workloads**: re-evaluation of expressions that haven't materially changed since the previous eval. Examples:

- `nixos-rebuild switch` running twice: <1% of derivations changed; without caching, both runs re-eval everything.
- CI `nix flake check` on every PR: 99% unchanged from main branch.
- Hydra evaluating jobsets: most of the input is identical run-over-run.
- Interactive `nix repl` refinement: same flake, slightly different queries.

If the hypothesis holds, **incremental result caching is a higher-leverage attack** than further within-run optimization, because:
- Within-run PIC ceiling: 2-3× per Hölzle/Ungar 1991 + JSC pre-DFG numbers.
- Cross-run caching ceiling: 10-100× per Bazel/Buck/Adapton existence proofs.
- Asymmetric leverage: caching skips work entirely.

**Verification status (§6 critique)**: I have no measured warm-vs-cold ratio for Nix workloads. No public dataset answers the question. The hypothesis is plausible from first principles (nixpkgs is deterministic, store paths content-addressed, eval is pure) but **must be measured before committing roadmap stages**.

## 2. Candidate Stage 10 — Incremental result cache (salsa-style)

### Goal (if hypothesis holds)

A persistent, content-addressed result cache. Keyed on `(cellHash, envHash) → resultBytes`. Built on top of Stage 9's cell store. Survives across process invocations.

### Architectural sketch

- **Cell hash**: already defined in `LINKING_DESIGN_2026-05-17.md` §2.1 (BLAKE3 of canonical IR).
- **Env hash**: structural hash of the free-variable bindings the thunk closes over. Bindings are themselves Values (potentially Thunks); hash their *expression* identity (body + free vars), not their forced value, to avoid forcing during hashing.
- **Cycle handling**: `letrec` / rec-attrsets introduce cycles. Two options:
  - Adapton-style `force_cycle` — programmer-supplied cycle value (§6 flags this is "practical, not provably sound").
  - v3-native: existing Blackhole detection already handles cycles; result cache simply doesn't store entries for cells observed in a cycle.
- **Cache invalidation**: keyed on (a) compiler-version salt, (b) opcode-table fingerprint, (c) impure-primop dependency tags (`currentTime`, `getEnv`, file content hashes).
- **Persistence**: extend Stage 9's SQLite schema with a `Results` table sibling to `Cells`.
- **LRU + size-threshold**: don't cache trivially-cheap values (cache lookup costs more than re-eval); size limit on persistent storage.

### Prior art (§6 verifies what's real)

- **salsa-rs** (rust-lang/salsa): used by rust-analyzer; **NOT** by rustc (rustc has its own red-green algorithm, salsa-inspired but distinct). Per §6, rust-analyzer's salsa 3.0 migration is currently struggling with memory regressions on a graph much smaller than nixpkgs.
- **Adapton** (Hammer/Khoo/Hicks/Foster, PLDI 2014, DOI 10.1145/2594291.2594324): paper real, theory grounded; **but** Rust crate documentation explicitly states cycle handling is a programmer-supplied fallback, not a soundness-preserving algorithm; no production deployments; last release ~2018.
- **comemo** (Typst): constrained-memoization library; production in Typst; not adopted in any Nix project.
- **Nix flake eval-cache** (Tweag 2020): exists at flake-output granularity in `~/.cache/nix/eval-cache-v4/`; per-invocation speedups reported (26× on `nix run firefox` repeat, 13.5× on `nix search nixpkgs blender`) — but these are at *attribute-path* granularity, not thunk-body; the gap to fine-grained caching is exactly what cppnix issue #6228 acknowledges and what no one has shipped.

### Estimated engineering scope (revised post-§6)

Initial estimate: 4-6 weeks. Revised after considering env-hash recursion through closed values, cycle handling, on-disk schema design, integration with primForce / forceValue, and validation across nixpkgs:
- Realistic: **8-12 weeks** for a robust implementation.
- Risk: rust-analyzer's struggles suggest the hard part is not "writing salsa" but "tuning memory + invalidation correctness at scale."

### Falsification criteria (Rule 0 compatible)

- **Pre-implementation kill**: measurement spike (§7) shows <20% warm fraction on real Nix workloads. If salsa wouldn't help much, don't build it.
- **Mid-implementation kill**: prototype on a synthetic workload (e.g. nixos-rebuild with 1-byte change) shows <2× speedup despite expected 10×+. Means cache granularity or env-hashing is wrong; revisit before scaling.
- **Post-implementation kill**: real-workload bench shows <3× on warm `nixos-rebuild`. The hypothesis is refuted; revert to interpreter-only with no result cache.

## 3. Candidate Stage 11 — Persistent attrsets (HAMT / CHAMP)

### Goal (if hypothesis holds)

Replace v3's flat-copy attrset representation with size-polymorphic persistent data structures: flat below ~32 entries, HAMT/CHAMP above.

### Motivation

The `prev // overlay final prev` pattern central to nixpkgs overlays runs N times during fix-point evaluation. With flat-copy, each `//` is O(left_size + right_size); structurally-sharing HAMT is O(log₃₂ n) per update. For 100 overlays on a 25k-entry pkgs attrset, the asymptotic difference is large.

### Verification status (§6 pushback)

- Steindorfer & Vinju OOPSLA 2015 (CHAMP) gains are on **iteration and equality** (1.3-6.7× and 3-25.4× respectively), NOT on insert/update.
- For small maps (<256 keys, which is "most nixpkgs intermediate attrsets" per common knowledge — though §6 notes no formal nixpkgs-shape study exists), flat-copy may beat HAMT due to:
  - Cache locality (flat is contiguous; HAMT is pointer-chasing).
  - Constant-factor overhead of HAMT operations (typically 2-3× slower than mutable Java for the small case).
- **No nixpkgs-shaped benchmark of flat-vs-HAMT exists in public literature.**

### Plausible design (if measurement supports)

Polymorphic-on-size representation:
- `Bindings` becomes a tagged union: `{ FlatVec, HamtTrie }`.
- Below a size threshold (initial guess: 32-64 entries based on the L1 cache line ≈ size of one HAMT node), use flat.
- On `//`-update that would exceed threshold, promote to HAMT.
- All attrset code paths (OP_ATTRS_*, primAttrNames, primMapAttrs, etc.) must handle both representations — significant migration cost.

### Estimated engineering scope

Realistic: **8-12 weeks**. Plus benchmark infrastructure to validate the design choice on real nixpkgs shapes.

### Falsification criteria

- **Pre-implementation kill**: measurement spike on real overlay-heavy workloads (haskellPackages, python3Packages with overrides) shows median attrset size <64 entries AND `//`-update cost is <5% of total eval time. If `//` is not a hot path, HAMT is irrelevant.
- **Mid-implementation kill**: synthetic overlay benchmark with size-1000+ attrsets shows <3× HAMT improvement. The expected 10⁴× best-case was correctly debunked in §6 — for realistic sizes, the wins are smaller.
- **Post-implementation kill**: real nixpkgs eval shows HAMT version is slower than flat. Polymorphic switch threshold is mis-tuned, or the hypothesis is refuted.

## 4. JIT discussion (deferred decision)

The literature is unambiguous: **interpreter-only PIC architectures cap around 2-3×** on attrset-heavy work (Hölzle/Ungar 1991, JSC pre-DFG measurements). For >3× cold-eval wins, the credible paths are:

1. **Truffle on Graal** (Würthinger et al. 2012) — partial-evaluation JIT; production-proven in JRuby+Truffle (30×), TruffleJS, FastR, GraalPython. **Requires Java rewrite** of v3.
2. **PyPy/RPython meta-tracing** (Bolz-Tereick et al. 2009) — tracing JIT for free from interpreter spec. Production in PyPy, Pycket (50× on R7RS micros), Topaz, Pixie. **Requires RPython rewrite**.
3. **Cranelift JIT backend** (Bytecode Alliance, in SpiderMonkey/wasmtime) — generate machine code from C++ via FFI; we write the JIT logic, Cranelift handles codegen. Production code generation; the JIT *logic* is on us.

**Honest assessment for Nix specifically**: Nix workloads are shallow (small data construction, few deep numeric loops). Tracing JIT (PyPy) optimizes deep loops, less applicable here. Method JIT (HotSpot) wants long-lived hot methods; most Nix thunks evaluate once. The intrinsic JIT win for Nix is smaller than for general-purpose dynamic languages.

**Therefore**: JIT decision is deferred until Stage 10 (if it lands) tells us what fraction of eval remains cold after caching. If post-Stage-10 the warm cases are fast and cold cases are tolerable, no JIT. If cold cases remain the user-visible pain, JIT is a real conversation — most likely as a fork (Truffle on JVM) since it requires host-language change.

## 5. Falsification table for the three candidate stages

| Stage | Hypothesis | Falsification trigger | Action on falsification |
|---|---|---|---|
| 10 (salsa) | Warm Nix eval is ≥50% of wall-time AND can be cached at thunk granularity with >5× speedup | Measurement spike <20% warm OR prototype <2× | Don't build; cold-eval optimization remains primary |
| 11 (HAMT) | Overlay-heavy attrsets in nixpkgs spend significant time in `//` AND size distribution justifies HAMT | Median size <64 OR `//` <5% of eval | Don't build; keep flat-copy |
| 12 (JIT) | Cold-eval cost remains user-painful post-Stage-10 AND PIC architecture has hit its ceiling | Post-10 cold eval acceptable | Don't fork to Truffle/PyPy |

## 6. Critical review — verification agent findings (2026-05-17)

A verification agent investigated the load-bearing claims in this strategy. Verbatim findings:

### 6.1 Workload distribution claim — UNVERIFIABLE

> "No public dataset quantifies what fraction of Nix eval CPU-time is spent re-evaluating expressions that are 'materially unchanged.' Nothing supports 60–80%, nothing refutes it. The number reasoned to is plausible but uncited."

Sources consulted: Tweag 2020 eval-cache blog, NixOS Wiki, SCALE 22x talk (Connor Baker), GitHub issues #2853/#4279/#6228/#9570/#10437, Hydra public stats, `nix-eval-cache` tool, Goldstein "Great Nix Flake Check" (the widely-quoted "96% hit rate" is about an HTTP proxy for flake-locking, NOT the eval cache — easy to misattribute).

### 6.2 Prior salsa-style attempts for Nix — none shipped

> "No one has shipped salsa-style fine-grained eval-cache for Nix. Tvix/Snix *defer* perf and intra-eval caching. Snix's 'finer cache granularity' refers to the **store layer (BLAKE3 content-addressed blobs)**, NOT to incremental eval inside the language VM."

Misreading Snix's "finer granularity" as eval-layer would have been a factual error in this proposal.

Sources: TVL Tvix August 2024 blog, snix.dev architecture, Lix issue #382 (parallel, not incremental), HNix `Nix.Cache` (no documented design), cppnix #6228 (open, zero implementation), `comemo` (Typst, not Nix).

### 6.3 Salsa-rs / Adapton production status — partial / weak

> "rust-analyzer: still uses salsa; mid-2025 actively migrating to salsa 3.0 — 'tests pass; correctness is good, but performance/memory usage has a concerning regression.' rustc: does NOT use the salsa library; uses red-green algorithm (salsa-inspired but distinct)."

> "Adapton: paper real (DOI 10.1145/2594291.2594324), theory solid, BUT cycle support is programmer-supplied fallback (not a soundness-preserving algorithm) AND no production deployments; last release ~2018. Building on it = research bet."

### 6.4 HAMT/CHAMP for small-map `//` — not a clear win

> "CHAMP gains are primarily on iteration and equality (1.3–6.7× and 3–25.4× respectively), NOT on insert/update. For small attrsets (most nixpkgs `recursiveUpdate` calls touch <50 keys), flat-copy `//` is often faster than HAMT due to constant factors and cache locality. No nixpkgs-shaped benchmark exists."

Sources: Steindorfer & Vinju OOPSLA 2015 (DOI 10.1145/2814270.2814312); Clojure/Scala HAMT production performance commentary.

### 6.5 Bottom line from verification

> "Do not commit a salsa stage to the v3 roadmap yet. Three reasons: (1) the 60–80% premise is unmeasured — instrument NIX_EVAL_REPEAT_PROFILE first on cardano-node / nixpkgs / hydra-eval-jobs to get an actual warm-fraction number; (2) the closest production validators (rust-analyzer salsa 3.0 migration) are struggling with memory regressions on a much smaller graph than nixpkgs would impose; (3) Adapton has no production track record and its cycle handling is programmer-supplied. The CHAMP swap should be similarly held until a nixpkgs-shaped microbenchmark exists."

## 7. Proposed immediate action — Measurement spike (1-2 weeks)

**Status**: this is the actionable conclusion of the strategy review.

Add a new sub-phase to the action plan (or a Stage 1.5 to the roadmap) with these deliverables:

1. **`NIX_EVAL_REPEAT_PROFILE` instrumentation in v3.** Track per-cell (or per-thunk-body) statistics:
   - Total forces per cell-hash across the run.
   - Cell-hashes observed in N consecutive `nix eval` invocations of the same workload.
   - Cell-hash result equality across invocations (would-cache-hit fraction).
2. **Three benchmark workloads** measured cold + repeated:
   - `(import <nixpkgs> {}).hello.name` — small.
   - Cardano-node flake `.packages.<system>.cardano-node` — medium fix-point overlay.
   - `nixos-rebuild dry-build` on a representative NixOS config — large module-eval.
3. **One overlay-heavy workload** measured at attrset-size distribution:
   - `(import <nixpkgs> {}).haskellPackages.<pkg>` with overrides applied.
   - Histogram of attrset sizes encountered.
   - Time spent in `//`-update primitives.
4. **One-page report** in `lode/MEASUREMENT_SPIKE_2026-XX-XX.md`:
   - Warm-eval fraction (refutes / partially supports / supports the §1 hypothesis).
   - Cache-hit fraction estimate.
   - Attrset size distribution.
   - `//` update cost fraction.
   - Verdict: does Stage 10 / Stage 11 commitment make sense?

### Why this is the right Rule 0 move

Per the falsification rule, the question "should we build salsa caching?" cannot be answered without measurement. The current evidence is insufficient. The 1-2 week spike turns the hypothesis into either:
- Killed (no salsa stage; PIC + linking are the right plan)
- Confirmed (salsa stage commitment with data-driven success criteria)
- Partially confirmed (commit a narrower version, e.g. flake-output-level only)

This is exactly the "kill a model OR confirm a model" exit per Rule 0, applied to architecture decisions, not just commits.

## 8. What would change the recommendation

- Measurement spike shows >40% warm fraction → **commit Stage 10**, sized to the measured potential.
- Measurement spike shows >20% time in attrset `//` AND size distribution skewed large → **commit Stage 11**, polymorphic threshold tuned to measurement.
- Measurement spike shows cold eval dominates → **don't commit Stages 10/11**; revisit Stage 6 (PICs) priorities.
- Stage 9 (linking) shows nixpkgs cell-dedup ratio <2× → Unison content-addressing hypothesis is also weaker than thought; revisit the whole content-addressed bet.

## 9. What to read

If the team is going to make this decision well, the relevant primary sources:

1. **Hammer/Khoo/Hicks/Foster, *Adapton: Composable, Demand-Driven Incremental Computation* (PLDI 2014)** — the theoretical foundation. Caveat: cycle handling is programmer-supplied per §6.
2. **Niko Matsakis on salsa** (rust-lang blog) — production reality of salsa in rust-analyzer.
3. **Steindorfer & Vinju, *Optimizing Hash-Array Mapped Tries* (OOPSLA 2015)** — CHAMP design. Caveat: wins are on iteration/equality per §6.
4. **Bolz-Tereick et al., *Allocation Removal by Partial Evaluation in a Tracing JIT* (PEPM 2011)** — what tracing JIT would buy us; whether we need it.

## 10. Cross-references

- **`ROADMAP_TO_VISION_2026-05-15.md`** — where Stages 10/11/12 would land if committed.
- **`LINKING_DESIGN_2026-05-17.md`** — Stage 9 substrate; salsa builds on the cell store.
- **`LESSONS_LEARNED_2026-05-15.md`** §4.1 — IR-duplication insight that underlies Unison.
- **`ACTION_PLAN_2026-05-15.md`** Rule 0 — the falsification principle this measurement spike applies.
- **`UNISON_IDEAS_2026-05-07.md`** §3 — hash-keyed eval cache; predecessor to the salsa proposal here.

Verification agent ID (continuable): `ad6ec5c5256587940`.
