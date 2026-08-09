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
- ❌ **`truncate-index` (a nix-tools tool) is NOT wired in** — the solver is handed the full
  pinned index, not a pruned candidate set. **Unused lever.**
- ❌ No `--max-backjumps` / solver tuning.

## The IFD catalog (what else there is)

Per project there is essentially **one** expensive IFD — the cabalProject plan **solve** —
plus a subsidiary **hadrian plan-nix** (a second, smaller solve for GHC's hadrian; shared
across projects on the same GHC) and sub-ms metadata reads (`index-state.nix` imports,
haskell.nix machinery `source` imports, `spdx/licenses.json`, `cabal.project` readFile). So
~1 real solve per project (×2 with hadrian); everything else is store-served noise.

## Levers to make the SOLVE faster (not caching the output; materialization REJECTED)

1. **Prune the candidate set** — wire `truncate-index` into the pipeline so cabal searches
   only the reachable dependency cone. Unused today; most tractable "genuinely faster cold
   solve" lever. **← WS-IFD-SOLVE variant (a).**
2. **Feed the freeze back** — persist the generated `cabal.project.freeze` per
   (project, index-state) and feed it in → the solve collapses to validation. Lighter than
   materialization (tiny, reviewable, still runs the solve so it catches drift).
   **← WS-IFD-SOLVE variant (b).**
3. **Solver tuning** — `--max-backjumps`, constraint ordering.
4. **Parallelism** — nej's per-leaf pool already runs independent projects' solves
   concurrently cross-job; the only serial point is within one eval.
5. **Moonshot** — a faster/incremental solver (SAT/SMT-backed, or warm-start from a prior
   plan). Large upstream effort; DEFER unless (1)/(2) both fall short of the 15% bar.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
