<!--
Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
-->
# RCA (OPEN) — systemic v3-direct drvPath divergence (2026-06-11)

## Symptom — CRITICAL, ship-blocking

Under pure v3-direct (`NIX_V3_DIRECT_EVAL=1`), **every** nixpkgs package's
`.drvPath` differs from the tree-walker. Confirmed by the authoritative harness:

```
test/run-759-nixpkgs-drvpath-sweep.sh --quick  →  0/8 pass, 7 fail
```

`hello git curl python3 openssl zlib ncurses jq ripgrep nix coreutils bash
gnumake cmake nodejs …` — ALL diverge. `python3` / `haskell.compiler.ghc98` even
fall back to `/v3-fake-store/…` (a secondary failure). Both nixpkgs 26.05 and
26.11 affected ⇒ universal, not version-specific.

Wrong drvPath = wrong store paths = v3 would build the wrong things. This is the
single most important v3 correctness bug.

## NOT the PAP fix (f5875a89c) — same-host-bisected

Reverting vm.cc+primops.cc to the PAP fix's parent (7358badda) and rebuilding gave
the **identical** divergent `hello.drvPath` (`rpj9…`). The divergence pre-dates the
PAP work.

## Localization (top-down, as far as it goes)

- **`derivation { … }` raw primop drvPaths are byte-IDENTICAL** (bare / with env
  attrs / fixed-output). So `derivationStrict` + the .drv serializer + the hasher
  are CORRECT. The divergence is in **stdenv-built derivations' eval-time attrs**.
- Walking the darwin stdenv bootstrap stage chain (`stdenv(.__bootPackages.stdenv)*`):
  - `bootstrap-tools.drv` — **IDENTICAL** (true leaf, 0 inputDrvs).
  - `bootstrap-stage1-stdenv` — **IDENTICAL**.
  - `bootstrap-stage-xclang-stdenv` — **DIVERGES** (first divergent stage).
- Every derivation's *recipe* (builder/args/env/system/outputs/inputSrcs) is
  **byte-identical after hash-normalization** at every level inspected (hello,
  hello.src, stdenv, clang-wrapper-boot, libiconv, gnugrep, gnused, cctools …).
  The divergence is ALWAYS in the **inputDrvs' output hashes** — a pure cascade.
- The divergence therefore enters with **packages built ON the (identical) stage1
  stdenv** — apple-sdk-14.4, llvm, cctools, libiconv, meson, ninja, gnugrep, … —
  whose mutual dependency graph is deep and fully interconnected.

## Why top-down localization is BLOCKED

1. **v3-direct does not persist intermediate `.drv` files** — `nix eval .drvPath`
   writes only the top drv; its computed input drvs are absent from the store, so
   `nix derivation show -r` / a drv-graph descent cannot reach v3's inner nodes.
   (Re-evaluating each input by ATTR works but the graph is deep and every hop is
   another cascade.)
2. **The 16B Value path is retired** (2544024bb) — so the cheap "swap value.hh +
   rebuild" bisect is invalid: the HEAD sources are 8B-only and a 16B-value.hh
   build produces `result is not a string (tag=2)` (broken, not a baseline).

## Leading hypothesis (UNCONFIRMED)

A **string-context divergence**: at some bootstrap derivation, v3 computes a
derivation env string with the same TEXT but a different string-CONTEXT (the set
of store-path references), so its inputDrvs/inputSrcs set differs → different
.drv → cascades to everything. (#682 is the canonical instance of this class:
"primToFile ignored contents' string context → wrong-hash drvPath".) The
"recipe text identical, only input hashes differ at every level" signature is
consistent with a context bug at the bottom that I could not reach top-down.

Plausibly introduced by **Lever B (Value 16→8B NaN-box, c690b3f19 / 2544024bb)** —
the only large eval-affecting change since the 2026-06-07 "M5/cardano byte-identical"
baseline; the regression window is {c690b3f19, 2544024bb, 4069a4cdb} (doc-only RCA
commits excluded). UNCONFIRMED because the bisect needs a full pre-Lever-B build.

## Next steps (pick one; each a real sub-effort)

1. **Confirm Lever B via a full pre-Lever-B build** — `git worktree add` at
   `c690b3f19^` (16B default), fresh `meson setup` + `ninja`, run #759. Green ⇒
   regression is in the 8B Value code (then diff the 8B codec / call sites,
   focusing on string-context propagation in derivationStrict's attr path).
2. **Build a "v3 persists intermediate drvs" debug mode** (or use `nix-instantiate`
   if it writes the full graph under v3) → unblock the drv-graph descent to the
   clean root.
3. **NIX_TRACE_EVAL differential** on the smallest divergent bootstrap derivation
   (e.g. boot `meson`/`ninja`/`libiconv`) — diff v3-vs-TW force/context order to
   pin the exact op that adds/drops a context entry.

## Repro pointers

- `NIX_V3_DIRECT_EVAL=1 build/src/nix/nix eval --impure --raw --expr
  '(import <nixpkgs> {}).hello.drvPath'` vs the same without the env var.
- `test/run-759-nixpkgs-drvpath-sweep.sh --quick` (0/8).
- bootstrap chain probe: `(import <nixpkgs> {}).stdenv(.__bootPackages.stdenv)*.drvPath`
  — stage1 IDENTICAL, xclang DIVERGES.
