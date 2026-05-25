# RCA Phase A7: post-fakeClo-fix downstream segfault — C-stack overflow
## via deep forceValue → dispatchLoop recursion in stdenv assertions

**Date:** 2026-05-11 (very late)
**Status:** Root cause identified; not a corruption — an architectural
limitation. Mitigation (depth-guard) landed; proper fix is iterative
forceValue or stdenv-side workaround.

## Trigger

After the Phase A6 fakeClo sentinel fix prevented the {family}
cell-stored-closure corruption (commit 1708d31bd),
`(import <nixpkgs> {}).hello.name` no longer returns the wrong value
but instead segfaults with `EXIT=139`. The same crash happens with
`NIX_V3_NO_CLOSURE_POOL=1`, confirming the closure-pool fix didn't
cause it — the previous {family} corruption was masking a deeper
issue by short-circuiting evaluation early at `OP_STR_CONCAT`.

## lldb backtrace

```
* thread #1, queue = 'com.apple.main-thread', stop reason = EXC_BAD_ACCESS
  frame #0:  forceValue          at vm.cc:7973
  frame #1:  dispatchLoop        at vm.cc:6338  ; OP_REC_BINDING_SLOT_REF
  frame #2:  forceValue          at vm.cc:8797
  frame #3:  dispatchLoop        at vm.cc:3051  ; OP_CALL (formals force)
  ... 3000+ frames of alternating forceValue / dispatchLoop ...
  frame #21: dispatchLoop        at vm.cc:7151  ; OP_CALL_PRIMOP
  frame #18: primDerivationStrictNative
  ...
```

Each forceValue → dispatchLoop pair burns one C-stack frame
(~1.4 KiB). At ~3093 frames, the 8 MiB macOS thread stack overflows.

## Where evaluation goes

With a depth-guard logging the frame stack at depth 2000:

**Bottom (origin):**
```
[0]  <root>     entryOffset = 17
[1]  pkgs       top-level/default.nix:217  ; `pkgs = boot stages`
[2]  stageFuns  booter.nix:42
[3]  thisStage  booter.nix:106
[4]  args       booter.nix:98
[5]  prevStage  booter.nix:86
[6]  prevStage  darwin/default.nix:1176 ; "no-op stage" assert wrapper
[7-19] darwin stage 1 packages + lib helpers
```

**Top (innermost recursion):**
```
[1999] v2                 lib/strings.nix:1956
[1998] v2                 lib/strings.nix:1990
[1997] <thunk>            python/cpython/default.nix:250
[1996] elems              lib/lists.nix:819
[1995] nativeBuildInputs  python/cpython/default.nix:236
[1994] nativeBuildInputs' make-derivation.nix:471
...
[1962] result             lib/customisation.nix:161
[1961] origArgs           lib/customisation.nix:159
[1960] <thunk>            top-level/python-packages.nix:2380
```

**Middle (every 100 frames):**
Almost every frame is python-package or make-derivation evaluation —
the eval is walking the python package set's dependency closure
even though `hello.name` doesn't transitively depend on python.

## Root cause: assertions in the "no-op" final stdenv stage

`pkgs/stdenv/darwin/default.nix:1174-1188` is the final stage of the
darwin stdenv bootstrap. Its body is a series of `assert` expressions:

```nix
(prevStage:
  assert isBuiltByNixpkgsCompiler prevStage.cctools;
  assert isBuiltByNixpkgsCompiler prevStage.ld64;
  assert isBuiltByNixpkgsCompiler prevStage.darwin.sigtool;
  assert isFromNixpkgs           prevStage.darwin.libSystem;
  ...
)
```

Each `assert isBuiltByNixpkgsCompiler prevStage.X` forces `prevStage.X`
to validate it. Some of those packages (e.g., python-deps via
`autoconf`/`automake`/etc reachable from `cctools`'s
`nativeBuildInputs`) pull in arbitrary subgraphs of the package set.

TW runs these same assertions and terminates fine. v3-direct goes much
deeper because **v3's forceValue is C-recursive** for thunk
evaluation: every `Tag::Thunk(Suspended)` force pushes a fresh
forceValue → dispatchLoop C-frame pair, vs. TW's in-place
forceValue update on the Value*. The thunk indirection chain that TW
walks iteratively (forceValue mutates `v` and re-enters the dispatch
loop) becomes a deep C-stack on v3.

3000+ thunks is unusual for `hello.name` but plausible inside the
recursive dfold/dependency-walk pattern: each package's `meta` /
`outputs` / `buildInputs` lives behind a chain of layered thunks
(`overrideAttrs`, `mkDerivation`, `callPackage`, `makeOverridable`,
`lib.attrsets.fix`, `mapAttrs` wrapping). Each layer is a thunk; v3
recurses one C-frame per layer; multiplied by the number of
dependencies the assertions transitively touch, we hit 3000.

## Mitigation (Phase A7): opt-in depth guard

`vm.cc:forceValue` entry now has an env-gated guard (`NIX_V3_DEPTH_GUARD=1`):

- Tracks `peakDepth` thread-local; logs every 100-frame step past 500.
- At depth 2000, dumps the bottom-20 / middle-stride / top-40 frame
  stack and `std::abort()`s. Abort (not throw) is intentional — a
  previous throw-based version of this guard caused 41 retries via
  `catch (...)` in v3_hook bridge paths, each leaking memory, eating
  20 GB before SIGKILL.

Default off so production runs see the original SEGV (and the C-stack
trace via lldb), not a synthesized abort.

## Why this isn't a v3 correctness bug

`(import <nixpkgs> {}).hello.name` works on TW. v3-direct overflows
C-stack but isn't returning wrong values — the recursion is processing
the legitimate stdenv assertion chain, just deeper than the C stack
allows.

The {family} bug (Phase A5/A6) was a correctness bug. This is a
scalability bug.

## Proper fix candidates

1. **Iterative forceValue.** The biggest payoff. Today forceValue calls
   dispatchLoop, which calls forceValue, ... — C-stack-bounded. An
   iterative loop that pushes a forceValue request onto a worklist and
   the outer dispatchLoop drains the worklist would cap depth at
   `kMaxCallDepth=5000` VM frames without C-stack growth.

2. **Increased thread stack.** Set thread stack to 32–64 MiB at
   process startup. Quickest workaround; doesn't fix the underlying
   issue. macOS default `pthread_attr_setstacksize` allows up to
   256 MiB.

3. **OP_RETURN chain collapse.** When a thunk body returns another
   Suspended thunk, eagerly evaluate the inner thunk (if cheap)
   instead of leaving the chain. Trades laziness for shallower
   chains. Risky — may evaluate values that should stay lazy.

4. **stdenv-side patch.** Move the final-stage assertions behind an
   opt-in flag (e.g., `__finalStage = stripAssertions args`) so v3
   evaluations bypass them. Pragmatic but couples nixpkgs to v3's
   limitations.

The right long-term fix is #1.

## What's safely working now

- The Phase A6 sentinel fix prevents the {family} corruption
  permanently. All future cell-stored Tag::Closure entries are safe
  from being silently re-purposed as fakeClos.
- All diagnostic env-vars from Phase A1-A7 remain in tree (gated
  off by default).
- The depth-guard (`NIX_V3_DEPTH_GUARD=1`) gives clean abort + stack
  dump when v3-direct hits pathological recursion, replacing the
  prior silent SEGV.

## Open issue (task #575 follow-on)

Iterative forceValue rewrite. Architectural change with broad impact;
requires audit of all callers (primops, OP_CALL_PRIMOP arg-force, OP_FORCE,
OP_BRANCH_FALSE condition force, OP_STR_CONCAT operand force, etc.).
Defer until prioritised against perf and other v3-direct work.

---

**RESOLVED 2026-05-18**. A7's C-stack-overflow workaround (depth-guard default-on, commit `3b59ed65d`) was rendered obsolete by Phase 1.2 iterative `forceValue` conversions (A8 series: `f6bf3fe8d`, `f82a2f725`, `5d9909d8c`, `f5804ea05`, `e1dfd98c2`). Depth-2000 abort removed in `377db9c16`. Phase 1 exit criterion (no C-stack overflow on hello.name) MET 2026-05-18 via Option 4 hybrid (commit `7adc7e61f`). See `OPTION_4_COMPLETE_2026-05-18.md`.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
