# IFD cost + first-eval decomposition — does a JIT matter for real workloads? (2026-06-29)

Follows REGISTER_VM_MEASUREMENT + the profile-workloads harness. Answers: are firefox/M5/HNE
eval-bound (JIT-relevant) or IFD/store-bound (JIT-irrelevant), and what is the one-time IFD cost.

## (b) IFD-build cost — cardano-node (M5)

cardano-node's IFD is `cabalProject'` → `cabal.project.drv` → `cardano-node-plan-to-nix-pkgs`,
and **requires x86_64-linux** ("a 'x86_64-linux' ... is required to build cabal.project.drv,
but I am aarch64-darwin"). On darwin-4 it builds in the apple-virt linux-builder VM.

Measured (deleted the cached plan-to-nix output, re-evaluated): the cold-store run was
wall=34.09s / user=1.21s ⇒ **~33s IFD build** (the wait), one-time. Re-eval with the IFD
cached returns to ~0 wait. So:

```
cardano-node TRUE first-eval (cold store): ~33s IFD (x86_64 cross-build) + ~12s cold eval
  (of which ~4.5s parse+lower) ≈ ~45s total
    → v3 eval ≈ 27% of first-eval; JIT-addressable run-phase ≈ 16%
cardano-node WARM re-eval (IFD + caches hot): 8.23s, wait 0.62s → 87% JIT-addressable
```

## Decomposition across the trio (darwin-4, WARM, from profile-workloads)

```
              v3 wall  evalCPU  wait  parse+lower(cold)  JIT-addr%(warm)  CPU×TW
  firefox      2.79s   1.90s   0.70s  0.87s             68%              2.57   (no IFD)
  cardano(M5)  8.23s   7.12s   0.62s  4.46s             87%              1.98   (IFD x86_64, ~33s 1-time)
  HNE          3.67s   2.82s   0.56s  4.24s             77%              1.82   (IFD, 1-time)
```

## (a) simplex-chat

WIRED into the harness: `packages.aarch64-darwin."exe:simplex-chat".name` (a real haskell.nix
flake, ghc963, /Users/angerman/Projects/zw3rk/simplex-chat — agent-confirmed). NOT cleanly
runnable on the available hosts: darwin-4 doesn't have the checkout; the laptop has it but its
IFD chain (cleanGit `git-ls-files` + x86_64-linux ghc963 plan-to-nix) isn't pre-built — the
laptop run got 24s into IFD builds then failed on a missing `git-ls-files` output. It is the
SAME IFD-gated class as cardano-node (heavier: ghc963, never built here). To measure: rsync to
darwin-4 + let the linux-builder VM build its IFD (slow, ghc963 uncached) — deferred; it would
re-confirm the cardano-node pattern.

## Verdict — JIT relevance is WARM-vs-COLD dependent

- **WARM / production re-eval** (hot caches + IFD built): EVAL-bound (68–87%). A JIT helps —
  but bounded (RCA: eval CPU only → 1.5–2.2× TW, doesn't beat TW). The laptop's earlier
  "M5 = half wait" was pure contention; the quiet host shows eval-bound.
- **COLD / first-eval / CI** (cold store): IFD-build dominates (~73% for cardano-node) +
  parse+lower (~11%) → a JIT addresses only ~16%. Here the levers are the IFD build (an
  x86_64-linux cross-build — infra, not eval) and the bytecode disk cache (parse+lower) —
  NOT a JIT.

So: pursue a JIT only if the target is warm/repeated eval. For cold/CI haskell.nix evals,
eval-engine speed is a minor slice; IFD + caching dominate. M5 IS an IFD workload, but its
warm steady-state is eval-bound, so it remains a valid JIT/eval benchmark for the warm case.

Harness: bench/profile-workloads.sh (firefox, cardano-node, HNE wired; simplex-chat slot +
SIMPLEX_EXPR, pending an x86_64-linux host with the IFD built).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
