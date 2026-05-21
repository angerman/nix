# v3 lowerNixExpr pathology on haskell-nix bootstrap.nix — 2026-05-22

## Status

**Root cause LOCALIZED, fix DEFERRED to focused session.**  The
interim default-rollback (`6cb4ecdb7`) makes cardano-node M5 work
default-on; this memo records the deep RCA work that diagnostic
instrumentation enabled.

## The bug

`v3::ir::lowerNixExpr(e, ns.symbols, ns.positions)` allocates 4+
GB while transforming `/nix/store/.../haskell-nix-source/overlays/
bootstrap.nix` (1275 lines) into IR.  Lowering never finishes
before the #753 RSS watchdog fires.

```
v3 IMPORT-MISS[48] RSS=116MB: .../overlays/bootstrap.nix
v3 IMPORT-PHASE before-lower RSS=116MB: .../overlays/bootstrap.nix
v3 SAFETY watchdog: process exit (137); RSS=4.1GB cap=4.0GB
  alloc: closures=7914 thunks=28 attrsets=5771 lists=903 pairs=77662
         insns=0 forced=0 bridgeForced=0
  bytes: v3_total=16MB boehm=384MB elsewhere=3.6GB
```

Note `insns=0` — v3 dispatch never starts.  `v3_total=16MB` —
the 4 GB is invisible to v3's allocator accounting; it lives
entirely in libc-malloc'd `std::vector<...>` growth INSIDE
lower's IR construction.

## What's been falsified

  * NOT optimisation passes — `NIX_V3_NO_OPT=1` reproduces.
  * NOT compile — never reaches compile (no "after-lower" log).
  * NOT a v3-native callFlake bug per se — repro is
    `builtins.typeOf (import "/nix/store/.../bootstrap.nix")` with
    or without v3-native callFlake.
  * NOT general "large file" — nixpkgs lib/lists.nix (similar
    size, complex shape) lowers in 57 ms / +1 MB RSS.
  * NOT a generic Nix pattern — synthetic stressors that all work
    in v3:
      * 500-element `let` (~60 ms).
      * 500-entry attrset literal (~52 ms).
      * 200-element `++` chain (~50 ms).

So it's some shape SPECIFIC to bootstrap.nix that v3 lower
mishandles.

## Where to look next

The synthetic stressors that work narrow the search.  Things
left unstressed:

  * `with` blocks combining attribute lookups.  bootstrap.nix
    uses `final.lib.optionals ...` heavily.
  * Recursive `let-rec` cross-references between bindings.
  * Conditional expressions inside attrset entries.
  * `inherit` + `inherit (X)` combined with `//`.
  * Deep `with` + path-`.` chains: `final.buildPackages.haskell.compiler ? ghc964`.

A binary-search bisection on bootstrap.nix's source would isolate
the construct.  Approach: extract the top-level `let` body, cut
in half, keep half that reproduces, repeat.  The Nix grammar
makes simple slicing painful (need to keep balanced braces); a
working starting point: comment-out individual let-bindings until
the pathology stops firing.

## Diagnostic infrastructure (landed this session)

  * **SIGALRM forensic dump** (`c9b91f455`) — async-signal-safe
    dump of alloc counts + top opcodes + Boehm/v3_total/elsewhere
    breakdown before `_exit(137)`.  Enables autopsy of runaways
    inside TW callbacks where in-dispatch poll can't run.
  * **`V3_DBG_GETFLAKE_RSS=1`** — RSS at each lockFlake +
    callClosure boundary in v3-native callFlake.
  * **`V3_DBG_IMPORT=1`** (enhanced) — RSS per import + phase-by-
    phase RSS within primImport (before-lower / after-lower /
    after-optimise / after-freeVars / after-compile / before-run /
    done).
  * **primImport module-scope tightening** — `module`'s IR
    vectors freed BEFORE recursive `run()`.  Hygiene fix, didn't
    itself reduce peak_rss (the pathology is upstream).

## Quick repro

```bash
F=/nix/store/fwpxh292ysfks28br7026nkz193p7l7q-source/overlays/bootstrap.nix
NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
  NIX_V3_MAX_HEAP=2G NIX_V3_MAX_WALL_TIME=10s V3_DBG_IMPORT=1 \
  build/src/nix/nix eval --impure --expr \
    "builtins.typeOf (import $F)"
```

(`fwpxh292ysfks28br7026nkz193p7l7q-source` is haskell-nix; the
store path will differ on other machines — locate via
`nix flake archive .#inputs.haskellNix` or similar.)

TW reference: ~73 ms.

## Workaround for users

Use the bridge path: `NIX_V3_NATIVE_CALL_FLAKE` is opt-in (default
OFF post-`6cb4ecdb7`).  Cardano-node M5 default-on works in 16.6 s
on a local checkout.

## Why this is hard to fix tactically

A "skip v3 lower for huge store paths" workaround was attempted
this session (bridge-store-imports approach via `evalFile +
treeWalkerToV3Public`).  It HUNG (10s wall-time exceeded with
RSS=31 MB) instead of OOMing — a different bug surfaces in
`treeWalkerToV3Public` of TW function values.  Bridging the
import path opens a separate set of issues (cycle in the
TW→v3 marshal of imported lambdas?) that needs its own
investigation.

Both bugs need to be fixed before v3-native callFlake can be
re-enabled default-on for haskell.nix workloads.

## Cross-references

  * `5af3dd1b1` (#753) — RSS watchdog.
  * `aadc35ae5` (#754) — cardano-node M5 bisection memo.
  * `6cb4ecdb7` (#755 rollback) — default flip-back.
  * `c9b91f455` (#755 diagnostics) — this session's instrument
    landing.
  * `lode/CARDANO_NODE_M5_2026-05-21.md` — M5 bisection context.
