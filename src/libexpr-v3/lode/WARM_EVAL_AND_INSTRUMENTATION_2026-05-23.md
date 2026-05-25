# Warm-Eval Framing + Instrumentation Cost — 2026-05-23

Two intersecting analyses captured together because they reshape the
perf-comparison framing:

1. **Unconditional instrumentation cost in v3.** What's compiled in
   regardless of env-var state, what it costs, what could be
   `#ifdef`-removed in a release build.
2. **The warm-eval / AOT reframing.** The user-facing scenario is
   **warm eval (cache load + execute) vs TW**, with AOT
   precompilation of nixpkgs / haskell.nix / other libraries
   explicitly acceptable. This changes the perf comparison
   substantially.

Companion to `GC_VS_TW_ANALYSIS_2026-05-23.md`,
`MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md`,
`OPTIMIZATION_STRATEGIES_2026-05-23.md`,
`PERF_AUDIT_2026-05-23.md`. Each previous doc measured
parse + lower + emit + execute combined. **This doc argues for
measuring execute-only as the primary user-facing target** and
identifies the release-build cleanup that pays in both wall and
memory.

## §1 — Unconditional instrumentation inventory

Per audit of `alloc.hh`, `vm.cc`, `closure.hh`, `run.cc`,
`barrier.hh`, `bridge_yield.cc`. **Compiled in regardless of env-var
state** — the cost is paid even when no diagnostic gate is set.

### §1.1 Always-on (every allocation pays)

| Site | File:line | What fires | Per-fire cost |
|---|---|---|---|
| Per-category byte counters | `alloc.hh:238-245` | `allocStats().bytesValues += sizeof(Value)` (and bytesClosures / bytesThunks / bytesEnvs / bytesLists / bytesBindings / bytesPairs / bytesChars on respective allocs) | ~1-2 ns + cache-line contention |
| Attrset size histogram | `alloc.hh:691-706` | 10-branch if/elseif cascade on `attrsetSizeBuckets[]` per Bindings alloc | ~2-3 ns (most allocs hit branch 1-3) |
| Allocation-count counters | `alloc.hh:128-135` | `pairsAllocated++`, `valuesAllocated++`, etc. per allocation | ~1 ns each, ~4-5 fire per typical eval cycle |
| Bindings origin recording call | `alloc.hh:713` | `bindingsAllocSiteRecord(b, file, line)` — early-returns if gate off but call+return costs | ~2 ns (branch + return) |

**Aggregate**: ~6-10 ns of pure instrumentation overhead per
Bindings allocation; ~3-5 ns per Thunk/Closure/ValuePair allocation.

### §1.2 Always-on (every dispatch pays, cheap)

| Site | What | Per-iteration cost |
|---|---|---|
| Dispatch-loop env-var gates | `static const bool s_x = std::getenv("X") != nullptr;` at function entry; `if (__builtin_expect(s_x, 0))` per check | ~0.5-1 ns per check, ~5-10 checks per opcode |
| `phaseDActive()` barrier-site check | `namespace inline const bool` per #767a; single byte load + branch | ~0.3 ns predicted-not-taken |
| Schema 10 IC slot read | One word read per `OP_REC_BINDING_SLOT_REF` | ~1 ns (cached) |

### §1.3 Always-allocated (no per-fire cost but ROM/data overhead)

| Item | Size | Conditional on? |
|---|---|---|
| `Thunk::forces` field | 4 B × ~1.6M Thunks = **~6.4 MB** | increment gated post-#733, but slot is permanent |
| `Thunk::shapeCell` field | 8 B × ~1.6M Thunks = **~12.8 MB** | alloc gated by `NIX_V3_CELL_EVERYWHERE`, but slot occupies struct |
| `LambdaDescriptor` cold fields (`forceCount`/`allocCount`/`callCount` 24 B; `name`/`contextualName` ~48 B + heap; `astLambda` 8 B; `posHandle` 4 B) | ~85 B × ~50K descriptors = **~4.2 MB** | mostly diagnostic / error-path |
| `allocStats().bigramCounts[256][256]` | 256 × 256 × 8 = **512 KB always-allocated** | per #780/#782; gated reads but static array |
| `attrsetSizeBuckets[10]` | 80 B | negligible |

## §2 — Estimated CPU cost when all gates are OFF

Back-of-envelope on hello.drvPath (~15M dispatches, ~6-7M allocations
per #778 + #719 measurements):

```
Allocation instrumentation:
  ~3-5 ns × 6.5M allocations = 20-30 ms

Dispatch instrumentation (gated checks only):
  ~0.5 ns × 15M = 7.5 ms

Phase D barrier check (fast path, off in common case):
  ~0.3 ns × ~5M barrier sites = 1.5 ms

Per-store byte counters within barriers:
  ~0.5 ns × ~1M = 0.5 ms

─────────────────────────────────────────────
Total estimated instrumentation overhead: ~30-40 ms
As fraction of ~895 ms hello.drvPath wall:    ~3-4%
```

**~3-4 % of wall is spent on always-on instrumentation that doesn't
change behaviour for production users.** Plus the ~25 MB of struct
slots permanently occupied + 512 KB of bigram array.

These numbers are estimates from per-line measurements + allocation
counts. Direct measurement would require a release-mode build to
compare against; that's part of the recommended next move (§6.1).

## §3 — What could be `#ifdef`-removed in release builds

Cleanly:

1. **All `allocStats().bytesX` counters** — only used by
   `NIX_VM_STATS=1` post-mortem dump. `#ifdef V3_RELEASE` removes
   all increments. ~2 ns per allocation reclaimed.
2. **`attrsetSizeBuckets[10]`** — only used for VM-2 sizing
   decisions during development. Compile-out the cascade. ~2-3 ns
   per Bindings alloc.
3. **`Thunk::forces` field (struct slot)** — increment gated, but
   slot permanent. Conditional-compile the field entirely. -6.4 MB.
4. **`Thunk::shapeCell` field** — gated by
   `NIX_V3_CELL_EVERYWHERE` (default OFF) but slot persists.
   `#ifdef` the field. -12.8 MB. **Already identified in
   `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` §2.1.**
5. **`LambdaDescriptor::forceCount/allocCount/callCount`** — only
   used by `V3_DBG_ALLOC_DUMP` atexit. Move to side-table or
   `#ifdef`. -1.2 MB.
6. **`LambdaDescriptor::name` / `contextualName`** — used only in
   error messages. Lazy-load from a side-table on first error
   (per Lazy materialization audit). -2-5 MB depending on string
   sizes.
7. **`bigramCounts[256][256]`** — 512 KB static array.
   `#ifdef V3_BIGRAMS_BUILD` to keep dev visibility but compile-out
   in release. -512 KB.
8. **`bindingsAllocSiteRecord` call at every allocBindings** —
   early-returns if gate off, but call+return still costs.
   Compile-out entirely. ~2 ns per Bindings alloc.
9. **95+ `V3_DBG_*` per-call diagnostic gates** in `lower.cc` /
   `vm.cc` / `primops.cc` — each adds ~50 B of inline code + one
   branch. Cumulative ~5 KB of i-cache footprint that competes
   with hot code.

**Aggregate release-build cleanup potential:**

- CPU: ~3-4 % of wall reclaimed (~30 ms on hello.drvPath)
- Memory: ~25-40 MB of permanent struct overhead reclaimed
- i-cache: ~5 KB more hot-code residency (small but compounds
  with the broader IC-broadening work in
  `OPTIMIZATION_STRATEGIES_2026-05-23.md` Tier 1)

This is **exactly the trade-off the team's `feedback_memory_first_class.md`
rule endorses**: small wall win + meaningful memory win for one compile
flag. The work is mechanical (~1 day to wire up, ~50 sites to gate).

**Critical caveat**: this only helps when env-vars are OFF. When
they're ON (development), the cost is intentional. This is a
**release-mode build configuration**, NOT a general optimization.
Development builds keep all instrumentation.

## §4 — The warm-eval / AOT reframing

This is the structurally important part.

### §4.1 What the current numbers actually measure

The team's published v3:TW = 1.41× wall on hello.drvPath
**includes everything**:

- Parse v3 / parse TW (both pay)
- Lower (v3 only — AST → IR)
- Emit (v3 only — IR → bytecode)
- Execute (both pay)
- Eval-state allocation (both pay)
- Disk-cache deserialize (v3 only — FAST post-#777/#781b at
  ~13.5 ms for 269 imports on hello.drvPath)

When the disk cache is warm (which is the default post-#777),
parse + lower + emit are **mostly bypassed** — the bytecode is read
from SQLite. The remaining cost on warm cache is deserialize +
execute. Per #781b: deserialize 333.8 ms → 13.5 ms via Schema 9
sparse symbolTable.

### §4.2 What TW does on warm cache

**TW has no equivalent per-import caching layer.** TW's
`~/.cache/nix/eval-cache-v5.sqlite` caches only **top-level
flake-output attribute Values**, not per-file bytecode or AST.
Every TW eval re-parses every imported file, re-builds the AST,
re-binds variable scopes.

**TW on warm cache pays full parse + AST construction every time.**
TW's cold-cache and warm-cache times are essentially identical for
the per-import work (only the flake output resolution is cached).

This asymmetry is hidden by the v3:TW = 1.41× wall comparison
because the team measures both v3 and TW with their own respective
caches. The numbers look balanced; the underlying work is not.

### §4.3 Decomposing hello.drvPath wall

```
v3 wall (warm disk cache, post #781b):  ~895 ms
  ├ deserialize:                          13 ms
  └ execute:                            ~882 ms

TW wall (warm flake-output cache):     ~673 ms
  ├ parse + AST build:                  ~100-150 ms (estimated)
  └ execute (tree-walk):                ~525-575 ms (estimated)

v3 execute-only / TW execute-only ratio: ~882 / 550 ≈ ~1.6×
```

The 1.6× execute ratio is slightly *worse* than the 1.41× headline,
which means warm cache is currently masking v3's execute overhead
(the AST-build cost shows up in TW). **Execute-only is the user-
facing number once compilation is fully amortized.**

### §4.4 Multi-tenant / CI asymmetry

hello.drvPath is a single-eval workload. The asymmetry compounds
when the **same imports get evaluated many times**:

- **TW**: pays parse + AST-build cost on EVERY eval, EVERY runner.
  CI farm with 1000 evals/day on shared nixpkgs = TW pays 1000×.
- **v3 with disk cache**: pays compile cost on FIRST eval per
  cache-key only. Subsequent evals are deserialize-only (~13 ms).
  CI farm = v3 pays ~1× for the same workload over the cache
  lifetime.

For Hydra-class workloads, **v3's amortized compile cost is
essentially zero; TW's is full**. This isn't visible in
single-process benchmarks but is the dominant factor for production
CI.

### §4.5 If AOT precompilation as deployment artifact is acceptable

Today's v3 disk cache is **first-eval AOT** — first run compiles,
subsequent runs read cache. **What's not happening yet: shipping
the cache as part of nix install / NixOS / haskell.nix releases.**

If we add that:

1. `nixpkgs-bytecode-cache` becomes a regular Nix package
2. Distributed via cache.nixos.org as a store path
3. NixOS / nix-env / installer fetches it on install
4. **First-eval-ever on a fresh box: zero compile cost.** Cache
   is already warm.

This would deliver two specific wins:

**(a) v3 cold-eval-on-fresh-box becomes warm-eval-on-warmed-up-box.**
The 1.41× ratio that includes any compile residue collapses toward
the execute-only ratio. v3:TW might close to ~1.3× or improve
slightly depending on cache-miss patterns.

**(b) Multi-tenant / CI scenarios become asymmetric in v3's
favour.** TW pays parse-AST-build on every CI runner. v3 pays
cache-load (~13 ms for hello.drvPath). For Hydra-class workloads
with thousands of evals/day on shared nixpkgs, v3's amortized
compile cost is essentially zero; TW's remains full.

### §4.6 What additional work would deliver this

Mostly already done. Specifically:

1. **Already shipped**: disk cache default-on (#777), sparse
   symbolTable (#781b), Schema 10 IC, content-addressed cache keys
2. **Architectural piece needed**: a `nixpkgs-bytecode-cache`
   package + distribution mechanism (cache.nixos.org binary cache
   integration). This is **Nix infrastructure work**, not v3-VM
   work — closer to the cache substituter protocol than to the
   evaluator.
3. **Optimization for shared-host scenarios**: mmap'd bytecode
   (per `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` §3.3 +
   the "elsewhere" agent E5 proposal) — let multiple eval
   processes share the same bytecode pages. 10-20 GB/day
   savings on CI farms.

Pieces 1+2 are mostly existing work + an operationalization step
(publishing the cache as a binary-cache artifact). Piece 3 is
multi-week but transformative for multi-tenant.

## §5 — How this re-prioritizes the optimization roadmap

The team's current optimization focus is **execute-path** work:

- `OPTIMIZATION_STRATEGIES_2026-05-23.md` Tier 1: broaden ICs
- `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` Tier B/C
- `GC_VS_TW_ANALYSIS_2026-05-23.md`: nursery default-on

These are all the right execute-path investments. **The warm-eval
framing doesn't change their value — it changes what comes alongside
them.**

If the user-facing goal is warm-eval-with-AOT-precompilation, then:

- **Execute-path wins still matter** — that's what the user pays
  per eval after compile is amortized. Tier 1 of OPTIMIZATION_STRATEGIES
  + Tier B/C of MEMORY_REDUCTION + nursery default-on all remain
  load-bearing.
- **Compile-path wins matter less** — the team's disk-cache +
  Schema 9 + #781b work already nearly-eliminates compile in warm
  scenarios. Don't double down on compile speed; it's already
  good enough.
- **Release-build instrumentation cleanup becomes higher-leverage**
  (§3) because it's pure execute-path overhead that hurts every
  warm-eval.
- **Mmap'd cross-process bytecode sharing** becomes interesting
  for multi-tenant CI scenarios.
- **AOT-precompile distribution mechanism** moves from "nice to
  have" to "the key infrastructure step" — but it's Nix-team
  work, not v3-VM work.

The strategic insight: **the team's been optimizing the workload
as currently measured (single-process, cold-include-compile), but
the user-facing scenario the team should optimize for is
warm-execute.** Most of the team's existing work helps both, but
the framing reshapes priorities and reveals the release-build
configuration as a higher-priority win.

## §6 — Recommended next moves

In order of leverage × cost:

### §6.1 Add release-build mode (1 day) — HIGHEST IMMEDIATE LEVERAGE

`V3_RELEASE` compile flag that `#ifdef`s out always-on
instrumentation per §3.

Expected:
- ~3-4 % wall reduction on warm-eval workloads
- ~25-40 MB permanent memory reduction
- ~5 KB i-cache footprint reduction

Effort: ~1 day to wire up, ~50 sites to gate.

Falsifier per `MEASURE_TWICE_CUT_ONCE`: build a release-mode binary;
run hyperfine ≥ 10 against today's binary on hello.drvPath +
cardano-node M5. Threshold: ≥ 2 % wall improvement OR ≥ 20 MB peak
RSS improvement to ship. If neither fires, kill (instrumentation
overhead was smaller than estimated).

### §6.2 Measure execute-only ratio (1 day)

Separate compile-time from execute-time in current benchmarks.
Publish v3:TW execute-only ratio alongside the wall ratio. The
team's measurement infrastructure already supports this (per #769
V3_TIMING per-import phase timing). Just needs the bench harness
update + a published table.

This is a **measurement spike**, not an optimization. Per
`MEASURE_TWICE_CUT_ONCE`: claims should be data-driven; today's
"v3:TW = 1.41×" is wall-only and obscures the execute-only
picture.

### §6.3 Update PERF_AUDIT + OPTIMIZATION_STRATEGIES with warm-eval framing (0.5 day)

Both documents currently center on the wall ratio. Add a §
"warm-eval as the primary user-facing target" to each. Cite §4 of
this doc as the rationale.

### §6.4 Spec out the bytecode-cache-as-package model (1-2 days)

Concrete proposal for how `nixpkgs-bytecode-cache` would be
built + distributed + consumed. Cross-team work (v3 + Nix
infrastructure + nixpkgs release engineering). The v3 side is
mostly already done; the infrastructure side is the work.

### §6.5 Cross-process bytecode mmap (2-3 weeks; defer until §6.4 commits)

The big multi-tenant win. Bytecode CUs become mmap'd-shared
across processes. ~10-20 GB/day savings on CI farms. Gates on
§6.4 because there's no point in mmap'ing if the cache isn't
shared as a deployment artifact.

## §7 — How this composes with prior work

The recent strategic synthesis points:

| Insight | Source | Relationship to warm-eval |
|---|---|---|
| Interpreter ceiling ~1.5-2× native | `OPTIMIZATION_STRATEGIES_2026-05-23.md` §9 | Holds for execute-only ratio. AOT removes compile noise but doesn't break the ceiling. |
| Cheap fruit on hello.drvPath exhausted (#783) | `OPTIMIZATION_STRATEGIES` + this doc | Confirms remaining wins are in per-op WORK CONTENT not dispatch. Warm eval doesn't change this. |
| Nursery default-on cascades through memory work | `GC_VS_TW_ANALYSIS_2026-05-23.md` §8 | Composes with warm-eval: nursery scavenges intermediate allocations that occur DURING execute; warm cache eliminates compile-time allocations. Different dimensions, both wins. |
| Memory wins compound differently than wall | `feedback_memory_first_class.md` | The release-build cleanup gives both. §6.1 is the canonical instance. |
| Bytecode + CU storage is a structural cost | `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` §3 | mmap'd shared bytecode (§6.5) is the answer — amortizes the cost across processes. |

## §8 — Honest limits

1. **§2's "~3-4 % wall" is an estimate** based on per-line cost
   times allocation counts. Direct measurement requires §6.1's
   release-mode build. If actual delta is < 2 %, the
   release-mode flag is still cosmetic and the work isn't
   critical — but the memory reclaim (~25-40 MB) likely still
   justifies it under `feedback_memory_first_class.md`.

2. **§4's TW parse-time estimate (~100-150 ms on hello.drvPath)
   is speculative.** No measurement. The team could derive this
   from running TW with `--time` or equivalent. Honest: the
   ~1.6× execute-only ratio derives from this estimate; if
   actual TW parse is ≤ 50 ms, the v3 execute-only ratio is
   worse than 1.6×.

3. **§4.5 AOT distribution requires Nix-infrastructure-team work
   not v3-team work.** The v3 side is mostly done. Whether the
   broader Nix project would publish a bytecode-cache binary cache
   is a different team's decision.

4. **The user-facing scenario shift assumes most production users
   are warm-eval users.** This is probably true (Hydra,
   developers iterating, CI runners with persistent cache) but
   first-time users on fresh boxes still pay cold-eval until
   AOT distribution ships. Cold-eval ratio remains ~1.4× from
   compile residue.

5. **The release-build mode adds maintenance burden** — every new
   `V3_DBG_*` gate needs to follow the release-mode convention.
   Worth documenting in the env-var inventory + CLAUDE.md.

## §9 — Cross-references

- `feedback_memory_first_class.md` — rule justifying the §6.1
  release-build cleanup (memory + small wall win)
- `feedback_measure_twice_cut_once.md` — §6.1 has the falsifier
  per the standard pattern
- `GC_VS_TW_ANALYSIS_2026-05-23.md` — companion strategic memo;
  GC analysis is execute-path; this doc is warm-vs-cold framing
- `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` §2.1, §3.3 —
  shapeCell + elsewhere overlap with §3 cleanup here
- `OPTIMIZATION_STRATEGIES_2026-05-23.md` §1, §9 — execute-path
  Tier 1 work + interpreter ceiling
- `PERF_AUDIT_2026-05-23.md` — sibling audit; this doc adds the
  warm-eval reframing
- #777 disk-cache default-on, #781b sparse symbolTable, #783
  super-instruction falsifier — recent measurement landings
  that ground §4's analysis
- #719 NIX_VM_STATS three-way RSS decomposition + #769 V3_TIMING
  per-import phase timing — the measurement infrastructure
  §6.2 leverages
- #733 instrumentation-gate fix — the canonical pattern for
  §3's release-build extension

## §10 — TL;DR

- **~3-4 % wall + ~25-40 MB memory of unconditional instrumentation
  cost** when all gates are OFF. ~1 day of work (#ifdef + release-
  mode flag) recovers this. **Highest-leverage immediate win.**
- **The 1.41× v3:TW headline includes compile residue.** Execute-
  only ratio is ~1.6× per estimated decomposition; needs direct
  measurement (§6.2 spike).
- **TW has no per-import caching layer.** Warm-cache scenarios
  amortize asymmetrically — v3 amortizes compile across runs;
  TW doesn't. Most visible in multi-tenant CI.
- **AOT precompilation as deployment artifact is mostly
  infrastructure work**, not v3-VM work. The v3 side is done;
  the binary-cache distribution piece is the missing link.
- **Mmap'd cross-process bytecode** is the transformative
  multi-tenant win, 2-3 weeks, gated on the cache being
  distributed as a shared artifact.
- **Warm-eval framing doesn't change execute-path priorities** —
  Tier 1 ICs, nursery default-on, Tier B/C memory reductions
  all remain load-bearing. It DOES elevate the release-build
  cleanup + bytecode-cache-distribution work that the team
  hasn't yet prioritized.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
