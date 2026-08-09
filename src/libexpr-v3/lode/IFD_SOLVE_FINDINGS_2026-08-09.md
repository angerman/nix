# IFD plan-build speed — findings (2026-08-09)

Empirical decomposition of "why is haskell.nix IFD planning slow", done to decide the
WS-IFD-SOLVE workstream. Measured on this workstation against real haskell.nix plans.

## The pipeline (two nix-tools custom executables)

`haskell.nix/lib/call-cabal-project-to-nix.nix` builds each cabalProject `plan-nix` in one
derivation that runs, in order:

1. **`make-install-plan`** (nix-tools) — the **SOLVE**: invokes cabal-install's Modular
   dependency solver ("Resolving dependencies…") with `-w ghc --enable-tests
   --enable-benchmarks --index-state=…`, producing `plan.json` + `cabal.project.freeze`
   (`call-cabal-project-to-nix.nix:700`).
2. **`plan-to-nix --full`** (nix-tools) — the **CONVERT**: `plan.json` + `.cabal` files →
   per-package `.nix` files (`:746`).

## The decomposition (measured)

| stage | tool | cardano-node (2051-pkg plan) |
|---|---|---|
| convert | `plan-to-nix --full` | **0.15 s** (with cabal-files; 0.03 s without) |
| solve   | `make-install-plan` (cabal Modular solver) | **~24.6 s** cold marginal (inputs warm) |

- The custom convert executable is **NOT the bottleneck** — it is essentially free (497
  `.nix` files in 0.15 s).
- The cost is the **cabal dependency SOLVE**, and it **scales with project size**
  (hello ~3.5 s → cardano-node 2051-pkg ~24.6 s marginal; ~78 s the first time, which
  also builds the one-time shared index tarball + GHC bootstrap).
- Cold marginal solve baseline (for the A/B): **24.6 s** (`nix build --rebuild` of the
  cardano-node plan drv, inputs warm; git-noted).

## What the pipeline already does — and doesn't

- ✅ Pins the index to `index-state` (`:140-166`) — bounds the candidate version space to a
  timestamp.
- ✅ Supports feeding a `cabal.project.freeze` (`:679-682`) — if provided, the solve is
  ~trivial. **But `cabalProjectFreeze` defaults to `null`** → every eval does a full cold
  solve by default.
- ✅ **`truncate-index` (nix-tools) IS already wired** — CORRECTION (WS-IFD scout, 2026-08-09):
  the original "NOT wired" claim was wrong (my grep only covered `call-cabal-project-to-nix.nix`;
  it is applied in `dotCabal`'s `postFetch`, `overlays/haskell.nix:258`, `truncate-index … -s
  ${index-state}`). It filters the hackage tar to `entryTime <= index-state` (its ENTIRE function,
  nix-tools `truncate-index/Main.hs:22-30`; flags only `-o/-i/-s`). It CANNOT prune to a reachable
  candidate cone — that needs the solve first (chicken-egg). So it prunes no more than the
  `index-state` pin already does. **Not an unused lever; fully exhausted.**
- ❌ No `--max-backjumps` / solver tuning.

## The IFD catalog (what else there is)

Per project there is essentially **one** expensive IFD — the cabalProject plan **solve** —
plus a subsidiary **hadrian plan-nix** (a second, smaller solve for GHC's hadrian; shared
across projects on the same GHC) and sub-ms metadata reads (`index-state.nix` imports,
haskell.nix machinery `source` imports, `spdx/licenses.json`, `cabal.project` readFile). So
~1 real solve per project (×2 with hadrian); everything else is store-served noise.

## Levers to make the SOLVE faster (not caching the output; materialization REJECTED)

1. ~~Prune the candidate set (truncate-index)~~ — **KILLED** (WS-IFD scout): already wired
   (`dotCabal` postFetch) and only filters by index-state timestamp; it cannot prune to a
   reachable cone (chicken-egg — needs the solve first). No win beyond the existing pin.
2. **Feed the freeze back** — the mechanism fully exists (`cabalProjectFreeze` auto-reads a
   committed freeze; the generated freeze is exported as `passthru.freeze`). A committed
   freeze collapses the cold solve to validation, drvPath-invariant (downstream keys on
   plan.json *content*, not the projectNix path — `load-cabal-plan.nix:7,117`). **But**
   auto-persist is chicken-egg (must solve once to get the freeze; by then the plan-nix
   output is already store-cached), and the deploy's steady state retains the plan warm in
   the local store anyway — so a freeze only helps truly-cold hosts / new index-state / GC'd
   store, and it is REUSE, not a first-solve speedup.
3. **Solver tuning** — `--max-backjumps`, constraint ordering.
4. **Parallelism** — nej's per-leaf pool already runs independent projects' solves
   concurrently cross-job; the only serial point is within one eval.
5. **Moonshot** — a faster/incremental solver (SAT/SMT-backed, or warm-start from a prior
   plan). Large upstream effort.

## WS-IFD-SOLVE VERDICT (2026-08-09) — pipeline levers KILL; moonshot DEFER

- **Variant (a) truncate-index — KILL.** Already wired + only prunes by index-state; can't
  do more (chicken-egg for a reachable-cone prune). Nothing to A/B; it is the baseline.
- **Variant (b) freeze-fed — KILL as a cold-solve speedup** (viable only as REUSE). A
  committed/cached freeze collapses the solve (drvPath-invariant), but that is reuse, and it
  is subsumed by the deploy's warm local-store plan retention; auto-persist is chicken-egg.
  Not a "make the FIRST cold solve faster" lever. (If the deploy wants the cold-host win, it
  is equivalent to caching the plan-nix outputs — which are NOT currently in the public
  caches — a store/ops choice, not a pipeline speedup.)
- **The floor is the cabal Modular solver's search** (make-install-plan; ~24.6 s cold
  marginal for cardano-node's 2051-pkg plan; the convert is free). The pipeline already does
  the tractable things (index-state pin + truncate-index + freeze support). A genuinely
  faster first cold solve requires a different SOLVER — **DEFER the faster/incremental-solver
  moonshot** (SAT/SMT or warm-start; large upstream effort) as an explicit, separate track.
- Correctness bar held throughout: any faster solve MUST keep the resolved plan drvPath
  byte-identical. Patch-scout detail: `ws-ifd-patch-draft.md` (this dir).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
