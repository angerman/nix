# v3 VM vs stock-nix tree-walker — perf/memory inventory (2026-06-03)

**Scope:** default-config **v3-direct** (`NIX_V3_DIRECT_EVAL=1`, no GC/evac)
vs **stock TW** across all known workloads. Wall = min over runs; RSS =
process max-resident. Same `nix` binary (HEAD `33654b071`), same host
(macOS aarch64), same nixpkgs per harness.
**Tools:** `bench.py --modes tw,v3-direct -n 5` (34 workloads, one clean
job → reliable ratios); `/usr/bin/time` + `NIX_VM_STATS` for firefox/HNE/M5.

---

## 1. Headline findings

1. **v3 is FASTER than TW on shallow nixpkgs attribute eval** — hello.name/
   pname/version/meta/system + attrnames-pkgs: **~0.50–0.57× wall (≈2×
   faster)**; firefox.name 0.71×. (Option-4 hybrid + caching.)
2. **Near-parity on deep derivation eval** — hello.drvPath **1.17×**,
   hello.outPath 1.39× (a massive improvement from the historical ~30×).
3. **2–3× SLOWER on tight arithmetic recursion** — fib33 3.1×, fib30 2.8×,
   ackermann-3-7 2.2× (pure interpreter+arena overhead, no caching benefit).
4. **Memory overhead is the consistent cost** — v3 uses **1.1×–5.7×** the
   RSS of TW everywhere; worst on recursion (fib33 5.7×) and deep
   derivations (hello.drvPath 5.6×). On heavy flakes ~4.2× (HNE/M5).
5. **Heavy-flake WALL is cache/state-dependent + noisy** (see §4) — the v3
   disk bytecode cache (cold=recompile=slow) swings M5 v3 wall 25–355 s.
   **RSS is the reliable axis there.**

Net: **v3 has reached wall-parity-or-better on the production-relevant
nixpkgs path, at a persistent ~1.5–5× memory cost** — confirming
[[memory-first-class]]: memory, not wall, is now the lever.

---

## 2. bench.py matrix (n=5, min wall / max RSS — reliable ratios)

### Synthetic / micro
| workload | TW s / MB | v3 s / MB | wall× | rss× |
|---|---|---|---|---|
| fib25 | 0.111 / 37 | 0.189 / 83 | 1.71 | 2.21 |
| fib30 | 0.486 / 122 | 1.349 / 593 | 2.78 | 4.84 |
| fib33 | 1.847 / 425 | 5.733 / 2407 | **3.10** | **5.67** |
| ackermann-3-7 | 0.214 / 76 | 0.464 / 243 | 2.17 | 3.20 |
| path-deep-30 | 0.069 / 29 | 0.076 / 32 | 1.10 | 1.11 |
| letrec-fix-5 | 0.071 / 29 | 0.073 / 32 | 1.02 | 1.11 |
| list-build-1k | 0.073 / 37 | 0.078 / 40 | 1.06 | 1.10 |
| fold-add-10k | 0.070 / 30 | 0.086 / 43 | 1.22 | 1.42 |
| with-deep-200 | 0.069 / 30 | 0.074 / 32 | 1.08 | 1.09 |
| attrset-build-1k | 0.071 / 29 | 0.080 / 33 | 1.13 | 1.11 |
| string-concat-1k | 0.070 / 32 | 0.076 / 35 | 1.08 | 1.11 |

### lib (nixpkgs lib functions)
| workload | TW s / MB | v3 s / MB | wall× | rss× |
|---|---|---|---|---|
| lib-foldl-1k | 0.071 / 30 | 0.077 / 37 | 1.08 | 1.23 |
| lib-foldl-10k | 0.071 / 31 | 0.092 / 47 | 1.30 | 1.50 |
| lib-genAttrs-100 | 0.073 / 30 | 0.075 / 36 | 1.02 | 1.21 |
| lib-mapAttrs-100 | 0.072 / 30 | 0.076 / 36 | 1.06 | 1.21 |
| lib-fix-deep | 0.069 / 30 | 0.075 / 36 | 1.10 | 1.21 |
| lib-recursive-update | 0.069 / 30 | 0.075 / 36 | 1.09 | 1.21 |
| lib-attrnames | 0.070 / 30 | 0.079 / 36 | 1.13 | 1.21 |
| lib-makebinpath | 0.083 / 30 | 0.081 / 37 | **0.97** | 1.22 |
| lib-evalModules-trivial | 0.084 / 31 | 0.082 / 40 | **0.97** | 1.28 |
| lib-evalModules-100 | 0.076 / 31 | 0.089 / 43 | 1.17 | 1.36 |
| lib-types-int | 0.083 / 31 | 0.088 / 39 | 1.05 | 1.27 |
| lib-strings-ops | 0.076 / 30 | 0.081 / 37 | 1.07 | 1.22 |

### nixpkgs packages
| workload | TW s / MB | v3 s / MB | wall× | rss× |
|---|---|---|---|---|
| hello-name | 0.608 / 130 | 0.302 / 216 | **0.50** | 1.66 |
| hello-pname | 0.583 / 130 | 0.307 / 216 | 0.53 | 1.67 |
| hello-version | 0.587 / 130 | 0.311 / 216 | 0.53 | 1.66 |
| hello-meta | 0.595 / 130 | 0.321 / 216 | 0.54 | 1.67 |
| hello-system | 0.601 / 129 | 0.327 / 216 | 0.54 | 1.67 |
| attrnames-pkgs | 0.596 / 129 | 0.337 / 216 | 0.57 | 1.68 |
| hello-drvpath | 3.577 / 139 | 4.171 / 780 | 1.17 | **5.62** |
| hello-outpath | 2.211 / 139 | 3.073 / 733 | 1.39 | 5.29 |
| derivation-chain-10 | 0.117 / 30 | 0.128 / 33 | 1.09 | 1.10 |
| derivation-chain-30 | 0.133 / 30 | 0.143 / 33 | 1.08 | 1.10 |
| derivation-strict-multi-output | 0.126 / 29 | 0.135 / 33 | 1.07 | 1.11 |

## 3. Heavy flake workloads (single clean run; RSS reliable, wall noisy)
| workload | TW s / MB | v3 s / MB | wall× | rss× | notes |
|---|---|---|---|---|---|
| firefox.name | 1.40 / 132 | 0.99 / 201 | 0.71 | 1.52 | v3 faster wall |
| HNE (haskell.nix hello.drvPath) | 55.4 / 572 | 33.2 / **2438** | ~0.6 | **4.26** | wall noisy; RSS stable (=R0) |
| M5 (cardano-node.name) | 34.3 / 851 | 162 / **3590** | noisy | 4.22 | v3 RSS 3590–4686 (R0 4686±102) |

Correctness: hello/git/HNE drvPath byte-identical to TW (verified separately).

## 4. Honest caveats (measurement integrity)
- **Two nixpkgs:** bench.py uses the flake's nixpkgs (`p5cm66…`); R0/noise-
  floor used `<nixpkgs>` (NIX_PATH) — different stdenv depth. (This is why
  bench.py hello.drvPath = 4.17 s but R0 = 0.98 s. The TW-vs-v3 *ratio*
  within bench.py is sound; absolute wall is nixpkgs-dependent.)
- **v3 disk bytecode cache** (default-ON; `NIX_V3_NO_DISK_CACHE` opts out)
  makes large-flake WALL highly state-dependent: cold = recompile bytecode
  = slow, warm = cached = fast. M5 v3 wall observed 24.6 s (R0, warm) →
  162 s → 355 s (contended/cold). **Heavy-flake wall needs a controlled
  warm/cold protocol for a definitive number; RSS is the stable metric.**
- bench.py wall = Python monotonic min over n=5 (not hyperfine); adequate
  for an inventory; headline ratios are stable to ~2 sig figs.

## 5. Where the memory goes (per [[project_post_f4_gc_r0_2026-06-02]])
v3's RSS overhead is the **v3 arena** (bump-allocated, not released by
default): hello.drvPath 587 MB arena (vs TW 139 MB total), HNE 1661 MB
arena, M5 7197 MB cumulative. This is exactly what the **Bartlett
evacuation GC (R2.4b)** targets — it already demonstrated HNE peak
2434→2151 MB (−12%) opt-in. Memory is the open lever; wall is parity-or-
better on the production path.

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
