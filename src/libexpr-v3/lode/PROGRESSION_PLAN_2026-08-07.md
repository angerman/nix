# v3 Progression Plan — from here (2026-08-07)

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0

Successor to the 2026-05-15 strategic set. Written after four cleanup/defect
passes + the `fix` (Nix-in-Zig) cross-check + the perf-lever arc. Load this and
`CLAUDE.md §0` before any new v3 work.

---

## 0. Where we are (the honest baseline)

**Correctness:** TW-parity is byte-identical on the real workloads (firefox/hello/git
drvPath). The one class of real divergences — coercion / integer-overflow / type-check /
comparison / parser error-behavior — was found by empirical `nix eval` v3-vs-TW testing,
fixed (~15 bugs across passes 3-4), and the deploy-relevant subset backported to
`iohk/angerman/2.34-v3` @ `815a041c1` (incl. the `builtins.div` INT64_MIN/-1 SIGFPE
worker-crash on x86_64). Two parity suites guard it: `run-coerce-parity` (53),
`run-arith-compare-parity` (37).

**Cleanliness:** dead-code floor reached (4 passes; audits now return mostly-refuted).
GC hardened — the 6-way object-child-layout re-encoding is now single-sourced by
`gc_layout.hh` with `static_assert(offsetof/sizeof)` pins (reshape → compile error) + a
post-scavenge manifest cross-check tripwire (silent missed-root UAF → loud abort).

**Perf — validated strategic map (confirmed independently by `fix`):**
- Single-eval CPU is near its ceiling: warm 1.8-2.4× TW, dispatch/alloc-bound. JIT ceiling
  1.4-2.2× and doesn't invert the gap.
- Parallel CPU eval is **DEFER** — the serial critical path (`lib/modules.nix` fixpoint +
  derivation-hashing DAG) is dependency-depth-bound; `fix` measured ~86% idle cores at w=32.
- Peak RSS is **representation-bound** ("peak RSS = live representation"); every Boehm-era
  reclaim lever died.
- Cold / CI eval is **IFD + parse-dominated** — and this is where the deploy actually spends
  time. **This is the only remaining frontier with real leverage.**

**Dead-ends already proven (do NOT re-tread — see §8):** all 5 `fix`-inspired perf levers;
the GC/RSS reclaim campaign; register-VM / super-instructions / PIC; the moat-store; Lever-1
deferred-per-attr compilation (KILLed at the Amdahl+warm-deploy gate).

## 1. Guiding principle

**Measure-first with a pre-committed kill criterion, every time.** This session repeatedly
showed prizes evaporate under scrutiny: audits over-report (~13/15 pass-4 findings refuted);
a 64.5-72.5% "wasted-compile" byte-ceiling collapsed to <10% of wall + 0 on the warm deploy.
The correctness gate is **empirical `nix eval` v3-vs-TW**, never code-reading. Kill a lever
cheaply *before* building it (Rule 0: a documented KILL is a deliverable).

The frontier is **IFD** (import-from-derivation) — the CI/nix-eval-jobs bottleneck. Two
orthogonal levers attack it (concurrency = overlap the *first* build; cache = skip the
*repeat*), and both are gated by one measurement (Phase 1).

---

## 2. Decision tree

```
Phase 1 (IFD tracing)  ── the one measurement that routes everything ──┐
   │                                                                    │
   ├─ material intra-job independent-IFD build-latency (net of the      │
   │  worker-pool cross-job parallelism)?  ──► Phase 2 (IFD concurrency)│
   │                                                                    │
   ├─ material repeat-IFD rate across evals (same output narHash)?      │
   │                                          ──► Phase 3 (IFD cache)   │
   │                                                                    │
   └─ neither material?  ──► IFD is not the lever; reconsider JIT       │
                              (Phase 4, warm-workload only) or STOP.    │
```

Phases 2 and 3 are independent and can both fire (they compose). Phase 4 (JIT) is a
different regime (warm, not the CI deploy) and is lowest priority.

---

## 3. Phase 1 — IFD tracing instrument (the foundation) · ~2-3 days · DO FIRST

**Why first:** it is the falsifier for Phases 2 and 3, it is valuable operationally on its
own (CI IFD visibility), and it is the cheap, low-risk, no-new-subsystem step.

**What v3 already has (WS-2, default-on — reuse, don't rebuild):** `OP_IFD_PROBE` bytecode
markers before IFD-class primops (`emit.cc:2036`, `#736` detector `emit.cc:1987`); per-kind
IFD-class read counts + context-bearing counts (`run_diag.cc`); realise-blocked total
wall-time (`nrIFDs`/`totalIFDTime`, `run.cc:447`).

**What to add** (gated instrument `NIX_V3_IFD_TRACE=1`, behavior-neutral, `--brute`-gated):
1. **Per-site attribution** — the callsite/`posHandle` where each IFD was hit (not just a total).
2. **Arguments** — the drvPath / derivation being realised (`ffi.cc:151 realisePath`), so a
   trace line identifies *what* was built.
3. **Per-realise wall-time** — time each `realisePath` individually (currently only a sum).
4. **Cache hit/miss** — did the v1 IFD/import cache serve it, or a real build?
5. **Independence signal** — for the IFDs in one eval, which were forced from mutually
   independent thunks (candidates for overlap) vs data-dependent (must sequence). This is the
   number Phase 2 lives or dies on.

**Measure on the real deploy shape:** a haskell.nix / cardano jobset via nix-eval-jobs (and a
broad `nix eval` proxy). Capture: #IFDs per job, intra-job independent-IFD build-latency,
repeat-IFD rate across a re-eval, IFD-build wall as a fraction of total job wall — **net of
the worker-pool's existing cross-job parallelism** (the pool already overlaps IFDs from
*different* jobs; the gap is *intra*-job).

**Deliverable:** the IFD cost profile that routes the decision tree. git-note the numbers.
**Kill/skip:** if IFD build-latency (intra-job, net of the pool) is a small fraction of deploy
wall, **both Phase 2 and Phase 3 are low-value** — stop and say so.

---

## 4. Phase 2 — IFD concurrency (latency-hiding) · gated by Phase 1

Overlap an IFD build (I/O-bound, seconds-to-minutes, often a *remote* darwin builder over
SSH) with continued eval. **This is distinct from the DEFER'd CPU-eval parallelism** — it
hides build *latency*, doesn't need CPU throughput, and `fix`'s idle-cores finding *supports*
it (spare capacity exists precisely while blocked on a build).

**Two options — pick by the Phase-1 data + effort appetite:**

- **2a — IFD build fan-out (narrow).** When forcing a list/attrset whose elements are
  IFD-thunks, batch-kick-off their builds so the daemon builds them in parallel, then eval as
  they complete. Directly targets the "5 sequential independent IFDs" case (= `fix`'s
  strict-demand fan-out). Needs a *bounded speculative discovery* of the independent IFDs
  ahead of forcing (lazy eval discovers them just-in-time — the hard part). Smaller, no
  general scheduler. **Preferred first attempt if Phase-1 shows the win is concentrated in
  list/attrset-of-IFD shapes.**
- **2b — futures + fibers + speculation (general).** Thunks-as-futures; forcing an
  IFD-blocked thunk parks the fiber and the scheduler runs other ready fibers; speculate on
  `import`/`fetchurl`/`derivation`. This is `fix`'s architecture verbatim — a proven reference
  design, but a large net-new subsystem. **Only if 2a is insufficient and Phase-1 shows a big,
  diffuse intra-job win.**

**The hard part (2b especially): GC-under-concurrency.** The always-on moving nursery + Phase-D
barriers were designed single-threaded. Concurrent forcing means concurrent allocation +
scavenge — a large correctness surface (`fix` uses TLA+ specs + ThreadSanitizer CI). Treat
this as the gating risk, not the scheduler.

**Gate:** darwin-4 + real-jobset wall, defer-off vs on, **net of worker-pool parallelism**.
**Kill:** realized job-wall win < ~10% after the pool already overlaps cross-job IFDs, or the
concurrency-GC safety can't be established → KILL (Rule 0). `--brute` under 1MB-nursery stress
+ firefox byte-id per step; flag-gated (`NIX_V3_IFD_CONCURRENT`), default-off until the gate.

---

## 5. Phase 3 — IFD cache (repeat-skip, "phase-3") · gated by Phase 1

Complementary to Phase 2: skip re-building/re-evaluating an IFD fragment on a *repeat* eval,
keyed on the **build-output narHash** — a *pre-eval* key (known once the derivation is
realised, before its content is evaluated), which is what makes an active cache-skip possible.

**History to respect:** the v1 IFD + import cache landed (Phase 4b, default-on). The v2
IFD-boundary fragment cache was **sound in shadow** but the **active use was KILLed — "a
post-eval key can't skip the body"** (chicken-and-egg). Phase-3's premise is that the pre-eval
build-output-narHash key breaks that. All three funding threads (moat-store NO-GO, JIT
GO-but-paired, parallelism DEFER) converged here.

**The crux is soundness**, not engineering: prove the build-output narHash is a *complete*
determinant of the fragment's eval result (nothing else in scope changes the value for the
same output hash), so an active skip can never return a wrong drvPath. Same bar that killed
moat-store.

**Plan:** (1) extend the Phase-1 tracing to log the pre-eval build-output narHash per IFD +
the fragment eval-result it would key; (2) **shadow mode first** — measure the would-be hit
rate on a CI re-eval + verify the key is a sound determinant (no false hits) against the TW
oracle; (3) **active only if** shadow proves both a material hit rate AND soundness.
**Kill:** shadow hit rate low, or the pre-eval key admits a false hit → KILL.

---

## 6. Phase 4 — JIT (CPU lever) · LOW priority for the CI deploy

Memory: JIT is **GO-but-paired** (Amdahl 1.4-1.7×; needs the alloc/dispatch reduction to
amortize), JIT-addressable 68-87% *warm*. But **cold/CI is IFD+parse-dominated, not
dispatch-bound** — so JIT helps the *warm / repeated single-eval* regime (interactive, dev
shells, `nix-eval-jobs` only where warm-cached), **not** the cold CI deploy that dominates
today. There's a `research/jit_copypatch_bench` spike in-tree.

**Do this only if** the priority use-case shifts to warm repeated eval, or after Phases 1-3
exhaust the IFD frontier. If pursued: copy-patch JIT (J3 needs no stack maps, per memory),
gated on a warm workload + the Amdahl-paired caveat, darwin-4 measured.

---

## 7. Cross-cutting / ongoing

- **Correctness maintenance:** the parity suites are the guardrail; any new v3-vs-TW
  divergence found → fix + backport the deploy-relevant subset to `2.34-v3` (the pattern that
  landed the div hotfix). Keep verifying empirically, never by code-reading.
- **Deploy hygiene:** the div-crash + parity fixes are on `2.34-v3` @ `815a041c1` but land in
  production only on your next `colmena apply` (+ ops-flake pin bump). Worth doing promptly for
  the crash fix.
- **Deferred low-value cleanup** (opportunistic only): `opt_*.cc`
  `chaseInBlock`/`remapExprVars` further dedup; the root-source-list single-sourcing (needs
  per-collector-aware care — missed-root risk, NOT plain cleanup).

## 8. Do-NOT-re-tread ledger (dead-ends, with why)

- **All 5 `fix`-inspired perf levers:** uncurried-arity + lazy-`//`-merge already shipped
  (superset); trivial-thunk-elision (~0.7% ceiling) + strictness-lattice (#776 falsified)
  tiny; deferred-per-attr-compile KILLed (emit only ~13% of cold pipeline; deploy AOT-warm →
  emit prize 0).
- **CPU-eval parallelism:** dependency-depth-bound (~86% idle cores at w=32, `fix`-confirmed).
  *(Note: IFD-latency concurrency, Phase 2, is a different axis — not covered by this.)*
- **RSS reclaim:** representation-bound; Boehm-tuning / Cheney / Immix / page-release /
  mid-eval-reuse / BiBOP all dead.
- **Register-VM, super-instructions, Stage-5 PIC, ChainBindings Phase C:** falsified.
- **Moat-store:** NO-GO on soundness (not engineering).
- **Trusting audits without empirical v3-vs-TW:** over-report; and watch the recurring
  **primop-vs-opcode split** (a fix to `primX` can miss `OP_X`, and vice-versa — div, head/tail).

## 9. Sequencing summary

1. **Now:** (ops) `colmena apply` the deploy backport for the crash fix.
2. **Phase 1 — IFD tracing (~2-3d).** The one measurement. Routes everything. Valuable alone.
3. **Then, per Phase-1 data:** Phase 2 (concurrency; try 2a before 2b) and/or Phase 3 (cache;
   shadow-first). Both gated, both killable cheaply.
4. **Later / different regime:** Phase 4 (JIT) only if the use-case turns warm.
5. **Always:** measure-first + pre-committed kill; empirical TW-parity as the correctness gate.

If Phase 1 shows the IFD frontier is smaller than believed, the honest conclusion is that
**v3 is done** — mature, correct, clean, deployed — and further eval-engine effort has poor
ROI until the workload or the store/build architecture changes.
