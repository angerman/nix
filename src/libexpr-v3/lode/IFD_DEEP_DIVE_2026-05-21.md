# IFD in v3 — Deep Dive — 2026-05-21

Four parallel agents reviewed: (a) the IFD problem in the Nix ecosystem
(web-research brief), (b) every IFD-aware code surface in this fork,
(c) the v3 architectural primitives and what novel handling they enable
(strategies S1-S7), and (d) a real haskell.nix IFD chain. Synthesis
below is organised around the **V3-NATIVE** principle — pure-VM path
preferred, TW only as the irreducible FFI leaf.

The central finding: **haskell.nix's manual `materialization` workaround
is already a content-addressed cache of post-IFD evaluator state. v3
can lift that pattern into the evaluator itself, automatically.** That,
combined with bytecode-level IFD detection + batching, captures the
highest-leverage wins without depending on the architecturally-blocked
"snapshot the entire eval continuation" path.

---

## 1. The IFD problem stated rigorously

From [the Nix manual][1]:

> Passing an expression `expr` that evaluates to a store path to any
> built-in function which reads from the filesystem constitutes Import
> From Derivation (IFD). … Evaluation can only finish when all required
> store objects are realised. Since the Nix language evaluator is
> sequential, it only finds store paths to read from one at a time.
> While realisation is always parallel, in this case it cannot be done
> for all required store paths at once, and is therefore much slower
> than otherwise.

cppnix is structurally two-phased (eval → realise) and single-threaded
in eval. When eval triggers a build mid-flight, eval *blocks* on
realisation, head-of-line-blocking everything else. As [jade.fyi][3]
phrases it: "when Nix is working on evaluating something that is
blocked on a build, it cannot go evaluate something else." The result
is head-of-line stalls that show up most dramatically in IFD-heavy
workloads (haskell.nix Cabal projects, Cardano/iohk infra). Default
policy in Nixpkgs/Hydra/flakes-with-`--no-impure` is to forbid IFD via
`allow-import-from-derivation=false` (PR [#5253][4]).

[1]: https://nix.dev/manual/nix/2.34/language/import-from-derivation
[3]: https://jade.fyi/blog/nix-evaluation-blocking/
[4]: https://github.com/NixOS/nix/pull/5253

---

## 2. Current ecosystem strategies (what's been tried)

| Project | Strategy | Trade-off |
|---|---|---|
| Nixpkgs / Hydra | Forbid IFD by default | Forces all code-gen committed upfront |
| haskell.nix (IOG) | Optional **materialization**: hash-pin IFD inputs, commit generated Nix | "PITA for newcomers, error-prone" ([issue #1366][7]); ~50 % eval-time win |
| haskell.nix (default) | Run `cabal-to-nix` as IFD on every eval | ~5 s/project floor even when cached |
| jade.fyi pattern | Coalesce all IFDs into one upstream derivation | One block, then fully cached; needs structural redesign |
| Hercules CI | Dispatch IFD builds back to the same scheduler | Still serial in eval; only scales across hosts |
| **Determinate Nix 3.11** | **`builtins.parallel`** — user-annotated parallel eval; threadsafe atomic thunk states ([Thunk → Pending → Awaited → Final][10]) | IFD test 50s → 10s; requires `builtins.parallel` calls in the source |
| Tvix (TVL) | Unified graph (no eval/build phase split) | Architectural intent; not shipped at scale per Aug-24 update |
| Lix | (no IFD-specific RFC found; inherits cppnix model) | — |
| nix-eval-jobs | Multi-process worker pool over attribute set | Each worker re-evaluates shared deps; no shared eval cache |

**Notable gap:** continuation-snapshot / fork-on-IFD / hash-based output
prediction appears genuinely **unexplored** in the Nix world. No RFC,
no implementation, no academic paper directly addressing it.

[7]: https://github.com/input-output-hk/haskell.nix/issues/1366
[10]: https://determinate.systems/blog/parallel-nix-eval/

### Build-systems analog: Buck2's `dynamic_output`

The structural problem ("analysis depends on a build artifact") is
identical to Nix IFD. Buck2's stance is explicit: it is *not* phased
("no target graph/action graph phases, just dependencies in a single
graph on DICE"). Its [`dynamic_output`][18] rule lets analysis read
the contents of a built artifact and bind further outputs from it.
Listed use cases (ThinLTO index, OCaml `ocamldeps`, Erlang headers)
map 1-1 onto Nix IFD. The crucial property: *only artifacts inside the
`dynamic` parameter block; independent targets keep building in
parallel.*

This is the design template Tvix is reaching for and that v3 — being
a bytecode VM with explicit dispatch state — can plausibly emulate
*within a single eval*, even without a full unified graph.

[18]: https://buck2.build/docs/rule_authors/dynamic_dependencies/

---

## 3. IFD in this fork today

Mapping from the codebase agent:

### 3.1 Pre-existing infrastructure (TW-side)

The fork already carries IOG's IFD-profiling patches deployed at zw3rk:

- `src/libexpr/include/nix/expr/eval.hh:42-50` — `IFDEvent`
  struct (drvPath, outputPaths, duration, status, pos, stackTrace).
- `:1127` — `realiseContext(context, paths, isIFD, triggerPos)`
  signature with explicit IFD-marking + source-position propagation.
- `:1237-1240` — counters: `nrIFDs`, `nrIFDsCached`, `totalIFDTime`,
  `ifdEvents` vector.
- `:1243` — `realisedDerivations` cache: `map<StorePath, StorePathSet>`,
  per-eval-session. **Memoed result: 14 → 4 IFD round-trips on
  haskell.nix.**
- `src/libexpr/primops.cc:72-300` — realiseContext implementation
  with cache check at :128, profile capture at :171-260, cache
  population at :286-301.
- `eval-settings.hh:220-222` — `profileImportFromDerivation` flag
  gates the per-event timing path.
- `src/libexpr/eval.cc:3620-3673` — JSON profiling dump (the
  `--option profile-import-from-derivation true` output).

### 3.2 v3-side handling — synchronous bridge only

Critical finding from the codebase agent: **v3 does NOT implement
realiseContext natively.** Every IFD-triggering primop in v3 bridges
synchronously to TW's `realisePath` and blocks the v3 dispatch loop
during the build:

| v3 site | Bridge target |
|---|---|
| `primops.cc:2780-2801` — `primPathExists` | `v3ToTreeWalker` + `ns.realisePath(noPos, tw, symRes)` |
| `primops.cc:3232-3255` — `primReadFile` | same |
| `primops.cc:7029-7044` — `primImport` when arg is a derivation | same (per the `#695 follow-on` comment) |
| `lower.cc:158` | side-effect comment acknowledging IFD is observable |
| `limits.cc:298-300` | "v3-direct has provable IFD-bounded wall time" — explicitly **deferred** to a later phase |

The consequences enumerated by the codebase agent:

- **No v3-side `realisedDerivations` cache.** If two v3 primops
  independently trigger IFD on the same derivation within one eval,
  TW's cache helps only when both crossings reach the same TW call
  graph; otherwise v3 bridges twice.
- **No v3-side `profileImportFromDerivation` gate.** v3 sees only
  aggregate counters via the shared EvalState. Per-event timing /
  stack traces are TW-only.
- **No v3-side batching.** Each primop → realisePath is independent;
  unlike TW's `buildPathsWithResults` (which collects multiple drvs
  into one batch), v3 bridges one at a time.
- **Wall-time accounting:** IFD wall time + memory count against
  `NIX_V3_MAX_WALL_TIME`, but v3 cannot distinguish IFD time from
  pure-eval time. The wall-time gate's retirement criterion
  ("provable IFD-bounded wall time") is explicitly deferred.
- **Stack-trace attribution:** v3 is invisible to TW's `debugTraces`
  when `realisePath` is called from a v3 primop bridge.

This is a textbook V3-NATIVE violation in the small (per-IFD
bridging) and an opportunity in the large (the synchronous block is
exactly where v3's bytecode/fiber/cache primitives could deliver
better-than-TW behaviour).

---

## 4. v3 architectural primitives applicable to IFD

The v3 architecture agent verified each capability with file:line:

1. **Fibers** (`include/v3/fiber.hh:59-100`, `fiber.cc` 249 LoC) —
   ucontext-based coroutines, dedicated mmap'd 16 MiB stack,
   exception forwarding, GC-safe via Boehm root registration.
2. **Reified call state** (`vm.hh:18-90`) — CallFrame with explicit
   `ip`/`closure`/`cu`/`stackBaseOffset`/`withStackBase`/`flags`.
3. **16-byte tagged Values + ThunkState** (`value.hh:32-100`,
   `closure.hh:82-95`) — five states including `Bridge` already
   exists for TW round-trips.
4. **Disk cache** (`disk_cache.cc`) — SQLite, content-addressed by
   SHA-256, WAL-safe. Already compiles + ships separately from
   cppnix's BytecodeDiskCache.
5. **Cheney nursery** (`nursery.hh`, `gc.cc`) — Phase A + C landed,
   gated `NIX_V3_NURSERY=1`. Phase D (write barriers) unresolved.
6. **NIX_TRACE_EVAL / heap-trace** — in-process force-order +
   heap-size sampler.
7. **Bridge thunks** — `Thunk::state == Bridge`, `bridgeSrc` points to
   a `nix::Value*`. **The IFD path already uses this.**
8. **Resource limits** (`limits.cc`) — `NIX_V3_MAX_HEAP` /
   `MAX_CPU_TIME` / `MAX_WALL_TIME` with typed exceptions.

---

## 5. Strategy space (S1-S7) — what's feasible, what's blocked

The architecture agent evaluated seven strategies. Below is the
distilled judgement, ordered by ROI / risk for V3-NATIVE:

### S5. Bytecode-level IFD detection (FOUNDATION — 3-5 days)

Insert an `OP_IFD_PROBE` instruction before any coercion / import /
readFile / pathExists in `lower.cc`. The dispatcher can then dispatch
differently (yield to scheduler, log, batch, query cache, …) without
each primop reaching into special-case logic.

- Feasibility: **High.** Pattern already exists for occurrence
  analysis in `lower.cc`.
- Soundness: **Very high.** Over-marking is conservative; the
  marker is a probe, not a constraint.
- V3-NATIVE: **Perfect** — dispatch stays pure bytecode.
- Prereq for S1, S2, S4, S6.

### S2. IFD batching — walk-and-collect (1 week)

Before the first OP_IFD_PROBE yield, run a reachability pass that
walks the bytecode of all reachable thunks and accumulates ALL IFD
markers. Submit them to `nix-build` in one batch (`--bulk-derivations
'[…]'`). Resume eval after the batch completes.

- Feasibility: **High.** Bytecode reachability is mechanical.
- Soundness: **High.** Dynamic IFDs (inside conditionals/loops) are
  conservative-marked; safe but not optimal. Second-wave IFDs that
  emerge during the build trigger a second batch.
- Upside: **5-20×** on cardano-node-scale evals where multiple
  IFDs naturally fire in sequence.
- V3-NATIVE: **High** — pure analysis pass + a single
  bridge call.

### S4. Content-addressed eval-result cache (1-2 weeks; THE BIG ONE)

Extend `disk_cache.cc`'s SQLite schema: add `(derivation_drvPath_hash,
primop_name, input_hash) → serialized Value blob` as a secondary key.
On OP_IFD_PROBE, query the cache first; on hit, deserialize the v3
Value and continue without building. On miss, build + cache.

- Feasibility: **High** — SQLite schema extension; Value
  serialization is the engineering challenge (closures, thunks,
  external refs).
- Soundness: **Very high** for deterministic builds; add a
  `--check-determinism` first-hit validation.
- Upside: **10-100×** on re-evals of the same workload.
- **This is haskell.nix's `materialization` workaround lifted into
  the evaluator.** Materialization stores `default.nix + plan.json +
  cabal-files/*` on disk keyed by `plan-sha256`. S4 stores the same
  result keyed by the same inputs, but at *evaluator* granularity,
  not at *Nix-file* granularity.
- V3-NATIVE: **Very high** — pure disk_cache extension; the only
  bridge surface is the Value-serializer.
- See §6.5 for why this is the headline recommendation.

### S6. Effect-typed primops (1 week — S4 enhancement)

Tag built-in primops with `EffectTag` (`Pure` / `StoreRead` /
`IFDTrigger`). The dispatcher takes different action on the
IFDTrigger class. Doesn't apply to user primops (unknown effect →
conservative "may yield").

- Feasibility: **Medium** (mechanical for built-ins; user primops
  default-conservative).
- ROI: **Composes with S2/S4** for fine-grained batching.
- V3-NATIVE: **Medium.** Slight drift if user primops force
  bridge-fallback.

### S1. Speculative parallel eval past IFD points (Stage 13+)

When v3 hits an IFD, spawn a sibling fiber on independent branches
that don't depend on the IFD output. Requires effect-tracking IR
pass to know which thunks don't depend on the build result.

- Feasibility: **Medium.** Effect-tracking is a 2-3 week IR
  investment, gated on S2 measurement showing the upside is real.
- Soundness risk: **High.** Wrong effect-tracking → race / wrong
  result.
- Determinate's `builtins.parallel` is the relevant prior art — it
  ships a thread-safe atomic thunk state machine (`Thunk → Pending →
  Awaited → Final`) that v3 could adopt without committing to
  effect-tracking, but Determinate's is *opt-in user-annotated*; the
  automatic version is harder.
- V3-NATIVE: **Medium.** Scheduler becomes a bridge point.

### S3. Continuation snapshot + persist (CRITICAL BLOCKER)

Snapshot the dispatch state (IP, frames, valueStack, withStack,
allocator arenas), release v3 memory, allow the build, restore from
disk after build completes. Would let cardano-node-scale evals
survive process restarts.

- **Architecturally impossible with current Boehm GC.** Boehm
  embeds pointers throughout the C stack; there is no "root set
  manifest" you can serialize. Restoring would require a *moving*
  GC so on-disk cell addresses translate to post-restore addresses.
- v3's tenured arena is still Boehm-conservative; the nursery
  (Phase A/C, opt-in) only handles young objects.
- A moving GC is not in the roadmap; Stage 11+ territory at
  earliest. **Do not attempt S3 until that lands.**
- This was also the genuinely-unexplored gap in the ecosystem
  research — for good reason.

### S7. Cross-process IFD cache (after S4)

Extend S4 to share the cache across processes (sqlite locking,
cache-coherence). Critical for CI parallelism where many eval
agents share the same workspace.

- Feasibility: **High** after S4 lands (SQLite already handles most
  locking; needs cross-process invalidation).
- Defer to Phase 2+ — S4 single-process gives ~80 % of value with
  ~1/3 the complexity.

---

## 6. Real workload grounding — haskell.nix IFD chain

The haskell.nix walkthrough agent established the actual shape of
IFD on a fresh `nix build .#hello-world` against haskell.nix:

### 6.1 IFD topology

Modern haskell.nix has *coalesced* into one big IFD via
`callCabalProjectToNix` — exactly jade.fyi's recommendation:

```
                 ┌─ hackage.nix index lookup (1×, mostly cached) ┐
                 ▼                                              │
   callCabalProjectToNix (1×)                                   │
     · `cabal v2-configure` in a derivation                     │
     · `plan-to-nix` on dist-newstyle/cache/plan.json           │
     · `cabal-to-nix` on every local + source-repo .cabal       │
     · output: { default.nix, plan.json, cabal-files/<>.nix }   │
                 │                                              │
                 ▼ ← imported as ONE IFD output                 │
   plan-nix attrset (hundreds of pkg entries) ──────────────────┘
                 │
                 ▼
   per-package callPackage of generated .nix files
   (no further IFDs in the common path; pure Nix from here)
                 │
                 ▼
   GHC pkg-set + Setup.hs derivations
```

So the **structure is already friendly to v3**: a single
synchronization point (the big IFD), then a wide independent fan-out.
The "14 → 4 round-trips" memo number measures secondary IFDs (source-
repository-package fetches, extra-hackage indexes, ghc-boot package
sets) coalesced by the realised-derivation cache.

### 6.2 Time-attribution profile (memo: 222 s plan-to-nix cold)

| Bucket | Est. share | Confidence |
|---|---|---|
| Hackage index download + parse in sandbox | 30-60 s | high |
| `cabal v2-configure` solver run | 60-120 s | high (super-linear on transitive deps) |
| `plan-to-nix` + `cabal-to-nix` sub-process per package | 30-60 s | high |
| **Nix evaluator wall-time waiting (blocking IFD)** | full 222 s | certain |
| Re-eval of held attrsets across IFD boundary | unknown | medium (best explanation for the 473 MB hello-world residue) |

### 6.3 Where v3 helps (mapped to strategies)

1. **Cache the post-IFD evaluated attrset, not just the store path.**
   The 473 MB attrset is re-parsed and re-evaluated on every
   invocation today; v3 can serialize the WHNF attrset keyed on the
   IFD inputs. **Lever: S4. Saving: 30-60 s steady-state on
   cardano-node** (the "5 s/project floor across hundreds of
   packages").
2. **Continuation persistence across process boundary.** Hydra/CI
   eval of `release.nix` takes 10-15 min. A crash today restarts
   from zero. Lever: S3. **BLOCKED** by Boehm GC.
3. **Parallel eval of independent package branches.** After the
   single big IFD resolves, the hundreds of `callPackage`
   invocations are independent. Lever: S1. Bounded by Amdahl on the
   still-serial big IFD itself.
4. **Lazy IFD output** — read only the attrs the consumer demands.
   Saves memory, modest wall-time. Lever: S1/S6.
5. **Batching the residual 4 IFDs** that haskell.nix doesn't already
   coalesce. Lever: S2.

### 6.4 The materialization workaround as existence proof for S4

This is the most important insight in the synthesis.

haskell.nix `lib/materialize.nix` checks `__pathExists materialized`
and if so, uses that directory instead of running the IFD build. The
materialized directory contains `default.nix` (the generated plan-nix
attrset) + `plan.json` + `cabal-files/<pkg>-<ver>.nix`. Its hash
mechanism (`plan-sha256`) makes it a **content-addressed cache of
IFD outputs**, keyed on the IFD inputs (cabal project text +
index-state + source-repo SHAs).

**This proves three things:**

1. **Content-addressed IFD caching is known to work** — haskell.nix
   ships it for production use.
2. **Substantial wall-time savings exist** — the "5 s/project floor
   on warm flake" vanishes when materialization is on.
3. **The only reason it's a manual workaround is that today's Nix
   has no automatic store for it.** v3 producing this automatically,
   keyed on the same inputs, is just lifting the workaround into
   the evaluator. **No new semantics are needed.**

The cache key haskell.nix uses (project text + index-state +
source-repo SHAs) is exactly what S4's `(derivation_drvPath_hash,
primop_name, input_hash)` resolves to. The Value-serialization piece
is the same problem haskell.nix's `default.nix` solves at the
source-code layer — v3 just does it at the WHNF-Value layer.

### 6.5 Why S4 is the headline recommendation

- It captures the dominant time-cost (re-eval of the post-IFD
  attrset).
- Its existence is already proven by materialization.
- It composes naturally with S5 (detection) and S2 (batching) but
  doesn't depend on them.
- It stays inside the V3-NATIVE pure-VM path: the cache is a v3-side
  SQLite + serializer, no TW round-trip.
- It is the only proposed strategy with both a real-world existence
  proof AND a single-process scope (no parallel-eval correctness
  risk).

---

## 7. Open questions / needs-measurement

From the haskell.nix walkthrough agent:

1. **What fraction of the 222 s is solver vs cabal2nix vs nix-eval-
   wait?** A real run with `--option profile-import-from-derivation
   true` on cardano-node would answer this. The IFD-profiling
   patches are already deployed; the numbers just need publication.
2. **What exactly forces the 473 MB hello-world residue?**
   `NIX_COUNT_CALLS=1 NIX_SHOW_STATS=1` should attribute it. Likely
   `compilerSelection` + spdx license tables, but unverified.
3. **Is there any *post-build-output* IFD that haskell.nix does**
   beyond `callCabalProjectToNix`? Walkthrough agent found none in
   the source survey but stack-cache-generator.nix may add some in
   stack projects.
4. **Which 4 IFDs remain after the 14→4 cache coalescing?** Memo says
   14→4, doesn't enumerate the 4. `profile-import-from-derivation`
   trace would name them.
5. **Does the realised-derivation cache invalidate correctly across
   `index-state` bumps?** Relevant for S4's key design.

---

## 8. Recommended roadmap

| # | Strategy | Effort | Risk | Falsifier |
|---|---|---|---|---|
| 1 | **S5** — emit `OP_IFD_PROBE` in `lower.cc` before coerce/import/readFile/pathExists; zero-cost when no IFD fires | 3-5 days | Low | dispatcher counts the probes; counter > 0 on a haskell.nix run |
| 2 | **Measurement spike** — run `--option profile-import-from-derivation true` on a fresh cardano-node + a fresh haskell.nix hello-world; publish the 222 s breakdown + the residual 4 IFDs | 1 day | None | data table feeds S2 / S4 design |
| 3 | **S2** — pre-pass that walks reachable bytecode, collects IFD probes, submits to `nix-build` in one batch | 1 week | Medium | wall-time delta on cardano-node; the residual-4 collapses to 1 batch |
| 4 | **S4** — extend `disk_cache.cc` schema with `(drv hash, primop, input hash) → serialized v3 Value`; Value-serializer is the engineering hard part | 1-2 weeks | Medium-high (serializer) | warm-eval wall time on hello-world (target: kill the "5 s/project floor") |
| 5 | **S7** — cross-process locking on the S4 cache; one CI pool shares warm cache | 1 week after S4 | Low | concurrent eval agents on Hydra don't re-IFD |
| 6 | **S6** — `EffectTag` on built-in primops; enables granular dispatch | 1 week | Medium | qualitative — opens design space |
| 7 | **S1** — speculative parallel eval past IFD via fiber + effect-tracking | 3-4 weeks | High | requires step 3 measurement to justify |
| — | **S3** — continuation snapshot | not now | **BLOCKED** by Boehm GC; re-evaluate after Phase D + moving-GC |

**Sequence:** 1 → 2 (measurement gate) → 3 → 4 (the headline) → then
5/6/7 driven by data.

### V3-NATIVE alignment summary

- S5, S2, S4, S7 keep the V3-NATIVE invariant: the VM never hands a
  thunk-cycle off to TW. The TW bridge surface is reduced to "ask
  nix-build to build these derivations" — i.e. the irreducible FFI
  leaf that the V3-NATIVE rule explicitly permits (store
  operations).
- S6 is V3-NATIVE-neutral.
- S1 introduces a fiber scheduler that *could* drift to TW if
  effect-tracking is wrong, but if implemented cleanly stays pure.
- S3 would deeply violate V3-NATIVE *if* it ended up serializing
  through TW; it doesn't anyway because Boehm GC blocks it.

---

## 9. The fork's existing IFD-profiling investment as accelerator

The fork already carries:

- `IFDEvent` struct + `ifdEvents` vector + JSON dump.
- `realisedDerivations` cache (14 → 4 round-trip improvement).
- `profileImportFromDerivation` flag.
- Per-event timing + position + stack-trace capture.
- Allocation attribution (`NIX_COUNT_CALLS=1`).

This is **half of step 2 (measurement spike) already done**. The
remaining work is plumbing v3 into the same events: have v3's
`OP_IFD_PROBE` emit an IFDEvent so v3-side IFD activity shows up in
the JSON dump alongside TW's. ~1 day to wire up; immediate cardano-
node visibility.

---

## 10. Honest gaps

- **Determinate's `builtins.parallel` is opt-in.** Its 50 s → 10 s
  win requires source code to call `builtins.parallel` explicitly.
  It is not a transparent IFD optimization; doesn't help unaltered
  haskell.nix. v3 could adopt its thread-safe atomic thunk state
  machine (`Thunk → Pending → Awaited → Final`) without committing
  to user-annotated parallelism, but that's S1 prerequisite work.
- **Tvix Aug-'24 status does NOT claim IFD is solved.** The
  unified-graph design [exists in the architecture document][12];
  shipped Tvix is not yet at scale on real workloads. Don't quote
  "Tvix solves IFD" without benchmarks.
- **Continuation snapshot is unexplored in Nix for a reason** — the
  GC story. The fact that *no one has shipped it* is consistent
  evidence of the architectural cost.
- **The 222 s number is 35 days old** (per memo). Re-measurement
  may show different bucket attribution post-Phase A landing.

[12]: https://hackmd.io/@NeqoUxq9SYSXDC7wNwihSA/B1eZFYlbi

---

## 11. Retiring haskell.nix `materialization` — the Unison program applied

(Added 2026-05-21 after a follow-up discussion. The S4 strategy in §5
is not a standalone idea; it is the first concrete user of the broader
content-addressed-evaluation program in
[`UNISON_IDEAS_2026-05-07.md`](UNISON_IDEAS_2026-05-07.md). That memo
was incorrectly tagged DEAD in `FORK_REVIEW_2026-05-21.md` §D; it is
the canonical reference for the work below.)

### 11.1 Materialization is a workaround for a cppnix limitation

Today's materialization workflow in haskell.nix:

```nix
project = haskell-nix.cabalProject {
  src = ./.;
  compiler-nix-name = "ghc966";
  materialized = ./materialized;   # ← the smell
};
```

A `materialized/` directory of generated `default.nix + plan.json +
cabal-files/<pkg>-<ver>.nix` lives in the repo, content-addressed by
`plan-sha256`. On eval, if `materialized` is set, haskell.nix reads
it from disk and skips the IFD entirely. The cost: humans run
`nix run .#materialize` after every `cabal.project` change; CI fails
loudly when the hash drifts.

The reason this exists is that **cppnix has no eval-result memory**.
Every `nix eval` re-runs every pure subexpression from scratch.
Materialization is a hand-rolled content-addressed cache: someone
runs the IFD once, commits the result, every subsequent eval reads
the commit instead of rebuilding.

The semantics it implements are pure caching:
- **Key**: hash of the IFD inputs (`plan-sha256` over project text +
  index-state + source-repo SHAs).
- **Value**: post-IFD-evaluated Nix source the consumer would have
  obtained.
- **Hit**: skip the build and skip the post-build eval.
- **Miss**: error (because materialization is a manual workflow, not a
  store).

### 11.2 v3 replacement = Unison Item 3 narrowed to IFD primops

`UNISON_IDEAS_2026-05-07.md` §3 (Hash-keyed evaluation cache)
specifies an evaluator-internal cache keyed on `(function_content_hash,
input_content_hash) → serialized result`. S4 from §5 of this document
is exactly this proposal narrowed to IFD-triggering primops as the
first scope:

| Materialization (today) | v3 cache (S4 = Item 3 narrow) |
|---|---|
| Key: `plan-sha256` over inputs | Key: `(drv-hash, primop, input-hash)` |
| Value: Nix source text on disk | Value: serialized v3 WHNF Value in SQLite |
| Hit: skip IFD build + skip post-build eval | Hit: skip IFD build + skip post-build eval |
| Miss: error (manual maintenance) | Miss: bridge to `realisePath`, cache the result |
| Storage: in-repo `materialized/` | Storage: `$XDG_CACHE_HOME/nix/v3-eval-cache.sqlite` (extension of `disk_cache.cc`) |
| Maintenance: human runs `nix run .#materialize` | Maintenance: none; cache key derives from inputs |

Same input → same output. Same trade-off. The only differences are:
- v3's cache is **automatic** (no `materialized` parameter, no
  `.#materialize` workflow); and
- v3's cache **degrades to a miss-then-build** instead of an error
  on key miss.

### 11.3 "Only hits on second eval" — three escapes

The single-machine cold-cache concern is real but mitigated:

**(a) It's never worse than materialization.** Someone always pays
the first-eval cost. Today that someone is "whoever ran
`nix run .#materialize`". With v3 cache, it's "whoever ran the first
`nix build`". The improvement is: they no longer have to *remember*
to update `materialized/` after a `cabal.project` change — the cache
key shifts automatically when inputs change.

**(b) Within a single eval session the cache fills as builds
complete.** A cardano-node eval that triggers 4 serial IFDs can have
IFDs 2-4 hit cache *within the same process* if they share inputs
(haskell.nix's `callCabalProjectToNix` output is consumed by many
downstream sites). Streaming cache population, not session-boundary.

**(c) Cache substituter — the real answer for teams.** Nix already
has the substituter protocol for store paths
(`cache.nixos.org`, `cache.iog.io`). Apply it to eval results:
- iohk CI runs one nightly eval and signs cache entries.
- Dev laptops + CI runners pull warm entries via the same fetch path
  as `/nix/store/*`.
- "Second eval" becomes "second eval **anywhere in the org**".
- Same trust model as binary cache (signed entries, public keys).

For haskell.nix users, the practical UX is:
1. Delete the `materialized = ./materialized;` line.
2. Configure the team substituter for `cache.haskell.nix/v3-eval` (or
   equivalent).
3. First eval on any new project pulls warm cache entries.
4. Never look at materialized/ again.

### 11.4 Sequenced program — Unison items mapped

Verified status (`grep` 2026-05-21):

| Unison item | Status in tree |
|---|---|
| Item 2 — ABT / α-equivalent identity | **NOT landed.** Despite the architectural-review memo claiming Stage 9 Phase L1, `grep` of `ir.hh` / `ir.cc` for de-Bruijn / ABT returns 0 matches. |
| Item 1 — Content-addressed IR fragments | NOT landed. |
| Shapes (Item 3 general-case prereq) | NOT implemented. `grep` for `struct Shape` returns 0. |
| Item 3 — Hash-keyed eval cache (narrow IFD scope = S4) | NOT landed. |
| Item 4 — Effect propagation / static IFD detection | NOT landed. |
| Item 5 — Hash-queryable cache CLI | NOT landed. |
| Disk cache at file-SHA granularity | Landed (`disk_cache.cc:86`, single-table SQLite). |

Concrete program to retire materialization:

| Phase | Item | Effort | Required for materialization retirement? |
|---|---|---|---|
| 1 | **`OP_IFD_PROBE` in `lower.cc`** (S5 from §5) | 3-5 days | Yes — gates everything downstream |
| 2 | **Disk-cache extension** — schema `(drv-hash, primop, input-hash) → serialized Value` (S4 = narrow Item 3) | 1-2 weeks | **Yes — this IS the materialization replacement** |
| 3 | **Cache substituter protocol** — sign entries, fetch like store paths | 1-2 weeks | Yes for teams with shared infra |
| 4 | **ABT** (Unison Item 2) | 1 week | Not for IFD-only cache (key is drv hash, a string); yes for cross-project sharing |
| 5 | **Content-addressed IR fragments** (Unison Item 1) | 1-2 weeks | Same — enables sharing across projects with different file paths but identical AST shape |
| 6 | **Effect propagation / static IFD detection** (Unison Item 4) | ~2 weeks | **The closer.** Lets `--option allow-import-from-derivation=false` fail at *lower-time* with precise call site, instead of runtime trap |
| 7 (opt) | Generalize Item 3 to all pure primops | 2-4 weeks | Needs shapes; goes beyond materialization scope |
| 8 (opt) | Hash-queryable CLI (Item 5) | 2 days after 1+2 | Pure ergonomics; helps debugging cache hit/miss |

**Phases 1-3 alone retire materialization for any team with a cache
substituter** — 4-5 weeks total. Phases 4-6 are the Unison payoff
layer that generalizes the win and converts the policy gate from a
runtime trap to a lower-time error.

### 11.5 Item 4 is the policy-shield replacement

Materialization is used for two distinct reasons:

1. **Perf**: skip the IFD's build + post-build re-eval. ← Phases 1-3 fix this.
2. **Policy shield**: in Hydra / restricted-eval contexts that forbid
   IFD, materialization is what makes the project *evaluate at all*.
   The forgotten-materialization bug surfaces as a deep-runtime IFD
   trap with no call-chain.

Item 4 (effect propagation) fixes the second reason. Quoting
`UNISON_IDEAS_2026-05-07.md` §4 verbatim:

> **Static IFD detection.** IFD has been a long-standing wish for
> static detection. As an ability — `RequiresStore` — it propagates:
> any function that transitively depends on `derivationStrict` is
> marked `RequiresStore`. Calling it in a context that statically
> forbids store access is a lower-time error with a precise call site.

The static check rides on the same machinery as `computeFreeVars()` in
`ir.cc:287-456`. At lower time, propagate `RequiresStore` upward; at
eval entry with `--no-allow-import-from-derivation`, fail with:

```
error: this expression requires store access (IFD)
  at flake.nix:42: callPackage ./pkg.nix {}
  → at pkg.nix:7: haskell.nix.callCabalProjectToNix { ... }
  → at haskell.nix/lib/call-cabal-project-to-nix.nix:N: derivationStrict ...
```

Today the same error fires at runtime, deep into the eval, with no
call chain. Item 4 gives the chain at lower time.

### 11.6 Migration path for haskell.nix specifically

A backward-compatible deprecation:

1. **Cut-in (week 0)**: v3 ships S4 cache + cache substituter.
2. **Deprecate (week 4)**: haskell.nix's `materialized` parameter
   continues to work AND its contents are read once as a *seed* for
   the v3 cache (the data is already content-addressed by
   `plan-sha256`; decoding it into the new schema is a one-time
   transform).
3. **Document removal (week 8)**: haskell.nix docs recommend removing
   the parameter from new projects; existing projects are unaffected
   but `materialized/` directories can be deleted from the repo with
   no functional change.
4. **Remove (much later)**: after a transition window, `materialized`
   becomes a no-op then is deleted from haskell.nix's surface.

Crucially: **existing `materialized/` directories are NOT wasted
work.** They become cache seeds. Users who already invested in
materialization get a smooth migration with no re-evaluation.

### 11.7 The Unison philosophy applied to itself

Nix already content-addresses derivations. The `/nix/store/<hash>-name`
pattern is the canonical instance of content-addressed storage in the
field. Materialization is a manual instance of the same pattern at
the source-code layer.

The Unison-ideas program **applies the same idea to the evaluator's
own outputs**: eval, like build, becomes a function whose inputs hash
to a stable key and whose outputs can be cached. No surface-level
change to the language; no UX change for end users. The cache hit/miss
is invisible — except in wall time.

This is "Nix philosophy applied to itself," as the original
`UNISON_IDEAS` memo §"Strategic note" puts it. Materialization is
the existence proof that the win is real; v3 lifts the proof into
the evaluator and removes the human burden.

### 11.8 Honest gaps (specific to this program)

- **First-cold-eval of cabal solver is unaffected.** The 60-120s
  cabal solver run inside the IFD's sandboxed builder is a derivation
  build — Nix binary cache territory, already solved if the inputs
  hash to the same drv. v3 eval-cache stacks ON TOP of binary cache;
  it doesn't replace it.
- **Cache invalidation across v3 versions** — every system that does
  this has had pain. Salt with opcode-table fingerprint (the pattern
  `disk_cache.cc` already uses for compiled bytecode) generalizes.
- **Non-deterministic IFDs would poison the cache.** Sandboxed IFD
  builds are Nix's default; `--check-determinism` validation on
  first hit per input combo catches the rest.
- **No one has shipped eval-result caching in Nix at this scope.**
  Tvix's unified-graph is architectural intent; Determinate's
  `builtins.parallel` is parallelism, not caching. Closest production
  analogs are Buck2's remote action cache and sccache's distributed
  compilation cache — same idea applied to compilation, not eval.
  v3 would be first.

### 11.9 What changes in §5/§8 of this document

§5's S4 description should be read as "the first concrete deliverable
of Unison Item 3" rather than a standalone strategy. §8's roadmap
items 4 (S4) and 5 (S7) absorb into Phases 2 and 3 of §11.4. Items
1-3 of §8 (S5 + measurement + S2) are unchanged.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
