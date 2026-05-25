## #815 cross-workload cache regression — RCA in progress (2026-05-25)

Living doc of the cross-workload disk_cache investigation.  Updated as
hypotheses are killed or confirmed.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.

## Reproducer

```bash
TMPCACHE=$(mktemp -d)
# Step 1: 5-pkg nixpkgs sweep populates the cache
for pkg in hello bash gcc python3 firefox; do
  NIX_V3_CACHE_DIR=$TMPCACHE NIX_V3_DIRECT_EVAL=1 \
    nix eval --impure --expr "(import (builtins.getFlake \"nixpkgs\") {}).${pkg}.drvPath"
done
# Step 2: haskell-nix-example FAILS
cd haskell-nix-example
NIX_V3_CACHE_DIR=$TMPCACHE NIX_V3_DIRECT_EVAL=1 \
  nix eval --impure --no-eval-cache --option allow-import-from-derivation true \
  --expr "(builtins.getFlake \".\").packages.x86_64-linux.hello.drvPath"
# → error: function 'anonymous lambda' called with unexpected argument 'git'
```

Workarounds that work:
- `NIX_V3_NO_DISK_CACHE=1` — disable cache entirely.
- Run haskell-nix-example on a cache populated only by haskell-nix-example.
- Production cache passes (specific stable mix from many sessions).

## Failure site

FORMALS-DIAG stack (post-#815-diag commit `168940499`):

```
[95] newArgs    customisation.nix:357:65  ip=2064
[94] result     customisation.nix:163:9   ip=1009
[93] origArgs   customisation.nix:161:7   ip=911
[92] newArgs    customisation.nix:177:11  ip=1070
[91] nix-prefetch-git'  fetch-cargo-vendor.nix:25:3  ip=192
[90] dep        make-derivation.nix:449:18  ip=5079
[89] <thunk>    make-derivation.nix:492:31  ip=6028
[88] pkg        attrsets.nix:1912:13 ctx=getOutput  ip=3683
[87..76] (string interpolation / firstOutPath chain into
        cargo-auditable-cargo-wrapper.nix:30:5)
```

The lambda being called is **lsdw9m87**.../nix-prefetch-scripts (no `git`
formal).  The args include `git=gitMinimal` (passed by `.override` at
fetch-cargo-vendor.nix:25).

## Two nixpkgs versions in play

  | path             | file               | git formal | gitMinimal formal |
  |------------------|--------------------|--------------|---------------------|
  | `77dbgds155b...` (user's `getFlake "nixpkgs"`) | nix-prefetch-scripts/default.nix | ✓ | — |
  | `lsdw9m87nam...` (haskell.nix's pinned nixpkgs) | nix-prefetch-scripts/default.nix | — | ✓ |

`final.path` in haskell.nix overlays resolves to **lsdw9m87** (haskell.
nix's own pin).  So `import (final.path + "/.../nix-prefetch-scripts")`
imports **lsdw9m87**'s version (no `git` formal).

User's nixpkgs (77dbgds) is loaded transitively via haskell-nix-example/
flake.nix's own `inputs.nixpkgs` AND via the system's flake registry's
`getFlake "nixpkgs"`.

## Confirmed observations

- 5-pkg sweep caches **user's** (77dbgds) nix-prefetch-scripts.  Cache
  KEY = sha256 of content; user's content has `git` formal.
- haskell-nix-example loads **haskell.nix's** (lsdw9m87)
  nix-prefetch-scripts.  Different content hash → DIFFERENT cache entry
  (not contaminated by user's variant).
- Single-pkg runs (hello / bash / gcc alone) on fresh cache do NOT
  contaminate haskell-nix-example.
- It's the **combination** of 5 pkgs that triggers the regression.
  Specific subset not yet identified (single-pkg bisection in progress).
- Per-workload runs (5-pkg sweep alone, haskell-nix-example alone) all
  pass cleanly.

## Open hypotheses

### H1 — Shared lib/customisation.nix CU corruption

5-pkg sweep caches lib/customisation.nix at its content hash.  If
77dbgds and lsdw9m87 have **identical content** for that file (which is
very likely — lib changes rarely between minor nixpkgs revisions),
they share a cache entry.  haskell-nix-example loads this shared
entry.  If the loaded CU's bytecode encodes scope/upvalue assumptions
that differ from what fresh-compile would produce, eval diverges.

**Test**: compare sha256(77dbgds/lib/customisation.nix) vs
sha256(lsdw9m87/lib/customisation.nix).  If equal, this hypothesis is
plausible.  If different, kill it.

### H2 — Symbol-id intern history affects per-Module Stage-4 strictness

Stage 4 strictness analysis is per-Module (no cross-CU global state),
but it walks the IR which uses SymbolIds.  If two processes compile
the same file with different SymbolId-intern orders (different earlier
imports), the analysis MAY produce different annotations.

This is conjectural; needs:
- Verify Stage 4's output is fully determined by Module content.
- Check whether emit's bytecode shape depends on intern order.

### H3 — Selector / identity peephole regression

Even though schema 13 fixes the cross-process remap of `selectorSym`,
maybe some OTHER process-local context bleeds through.  Audit other
LambdaDescriptor fields that are NOT serialised.

### H4 — IFD-import EvalResults cache contamination

The IFD-import disk cache (EvalResults table) caches VALUES, not just
CUs.  A value cached by 5-pkg sweep MAY hold a Closure whose desc
points into the writer process's CU pool.  Reader loads the Value but
the Closure-desc pointer is dangling / process-local.

**Earlier test**: NIX_V3_NO_IFD_IMPORT_CACHE_DISK=1 did NOT fix the
regression.  Likely killed.

### H5 — Cache POPULATES a shared CU that haskell-nix-example mis-uses

The 5-pkg sweep imports nixpkgs's top-level / .../all-packages.nix.
haskell-nix-example imports many of the same standard library files.
If one of these shared CUs has a bytecode that ENCODES the writer's
overlay context (e.g. captures `pkgs` references somehow that differ
between workloads), the loaded CU misbehaves.

Plausible.  Needs: enumerate which CUs are SHARED between the two
workloads, then check one-by-one which one drives the divergence.

## Diagnostic next steps

1. **sha256-diff comparison** of all overlapping nixpkgs files between
   77dbgds and lsdw9m87.  Identify shared content-hash files.
2. **Cache entry enumeration** post-sweep: list every CU in the cache.
   Compare against the files haskell-nix-example actually imports.
3. **Per-CU deserialize-and-compare diagnostic**: load each cached CU,
   immediately re-compile from source, compare byte-by-byte (after the
   sort+remap normalisation).  Any mismatch = bug location.
4. **NIX_V3_NO_DISK_CACHE_PATH=<substr>** opt-out per-path: skip
   cache for one specific path at a time; identify the offending CU
   via process of elimination.

## Status

Investigating.  Re-opened after the schema-13 commit `ed8fa0669` was
believed to resolve #815 but didn't — production cache happens to
pass; fresh tmp + 5-pkg sweep + haskell-nix-example still fails.
