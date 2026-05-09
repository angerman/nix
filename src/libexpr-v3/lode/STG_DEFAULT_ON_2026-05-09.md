# STG default-on landed — 2026-05-09 (#547)

## Outcome

`NIX_V3_STG=1` semantics are now default-on for the five runtime
gates: publish (vm.cc), side-table recovery (vm.cc x2), eval-hook
(v3_hook.cc), call-hook (v3_hook.cc).  Polarity inverted to read
`NIX_V3_NO_STG` instead of `NIX_V3_STG`.  Set `NIX_V3_NO_STG=1` to
restore the legacy publish-and-recover path.

## Validated inventory matrix (post-flip)

```
workload             | tw          | v3-direct | v3-direct-stg | v3-fhook  | v3-fhook-stg
---------------------+-------------+-----------+---------------+-----------+-------------
rec-simple           | OK          | OK        | OK            | OK        | OK
rec-self-dot         | OK          | OK        | OK            | OK        | OK
let-prev-update      | OK          | OK        | OK            | OK        | OK
let-rec-mutual       | OK          | OK        | OK            | OK        | OK
formals-default      | OK          | OK        | OK            | OK        | OK
formals-mutual       | OK          | OK        | OK            | OK        | OK
lib-id               | OK          | OK        | OK            | OK        | OK
lib-fix-simple       | OK          | OK        | OK            | OK        | OK
lib-extends-1        | OK          | OK        | OK            | OK        | OK
lib-extends-with     | OK          | OK        | OK            | OK        | OK
lib-makeextensible   | OK          | OK        | OK            | OK        | OK
nixpkgs-typeof       | OK "set"    | CYCLE     | TIMEOUT       | OK "set"  | OK "set"
nixpkgs-lib-id       | OK 42       | CYCLE     | TIMEOUT       | OK 42     | OK 42
nixpkgs-hello-name   | OK "hello"  | CYCLE     | (killed)      | OK "hello"| OK "hello"
```

Headline: `v3-fhook` (the user-visible mode after `NIX_USE_V3=1`)
is now FULLY GREEN on full nixpkgs — `typeof`, `lib.id`, `hello.name`
all match TW.  Before the flip these were `BLACKHOLE`.  The STG
default-on flip fixes them transparently with no env-var changes
required from users.

## What this fixes

- `(import <nixpkgs> {}).hello.name` under v3-fhook: was BLACKHOLE,
  now matches TW.  The legacy publish was writing wrong-shape
  intermediate values (a size-1 `{prev}` recAttrs from `lib.extends`'s
  `let prev = f final`) onto outer Black thunks, surfacing as
  blackhole / cycle / not-found errors at lookup time.

- `f 35` (fibonacci, no rec attrs): was 10.48s, now 6.40s on the
  reference workstation (REVIEW_2026-05-08.md §perf).  The legacy
  publish had a per-call cost the slot mechanism doesn't pay.

## What this does NOT fix

- `(import <nixpkgs> {}).hello.name` under **v3-direct**:
  pre-existing eval-order divergence — v3-direct triggers a force on
  a `with pkgs;` thunk DURING construction of pkgs (specifically a
  `recurseIntoAttrs` call that eagerly forces its `attrs` parameter
  via OP_GET_LOCAL_FORCE).  TW never reaches that force during
  construction.  Frame trace + bytecode analysis lives in
  `CALLPACKAGE_BUG_2026-05-09.md`.

  STG default-on doesn't change v3-direct's failure (still a clean
  cycle, NOT a regression).  Tracked separately as #547 Phase 3.

## Why the lower-time gate stays opt-in

`lower.cc:1624` (STG-5: blanket inherit-from thunkify, matching TW's
`from->maybeThunk`) is gated `NIX_V3_STG=1` opt-in even after the
runtime flip.  Flipping it default-on triggered a SIGSEGV on full
nixpkgs (vs. the clean cycle error on the runtime-only flip),
suggesting a secondary issue with the IR shape it produces that
needs separate hardening.  The lower-time change generates more
MkThunk nodes, and one of those interacts badly with v3-direct's
eval-order divergence.

Re-flipping it is part of the v3-direct fix work.

## Acceptance criteria met

- All synthetic + lib-only workloads pass under v3-direct and v3-
  fhook in both STG-default and `NIX_V3_NO_STG=1` modes.
- v3 unit test suite (smoke / drv-preflight / evalscope / bench /
  lint / let-rec-publish-split): 6/6 OK.
- `let-rec-publish-split` regression suite: 11/11 OK.
- `inventory-stg-mode.sh` matrix: STG default = STG explicit
  (modulo the lower.cc backout for direct-eval safety).

## Next steps (sketched, not in this commit)

1. **Delete legacy publish code** — once we're confident in the
   default-on flip across more workloads (a few release cycles of
   "no one set NIX_V3_NO_STG=1"), the entire
   `publishToNearestBlackThunkFrame` body + `partialBindingsRegistry`
   side-table can be removed.  The `OP_ATTRS_LET_REC_INIT` split
   from #546 also becomes equivalent to OP_ATTRS_REC_INIT and can
   collapse back.

2. **Fix v3-direct eval-order divergence** — find what makes v3
   force `recurseIntoAttrs (callPackages …)` during construction,
   when TW doesn't.  Most likely a lowering or emit-time
   thunkification gap.  Frame trace already pinpoints the failing
   thunk's body (codeOff=45693, OP_WITH_LOOKUP for `callPackage`)
   and its caller (the `attrs` lambda = recurseIntoAttrs).

3. **Re-flip lower.cc:1624** once #547 Phase 3 completes the
   eval-order fix.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input
Output Group.
SPDX-License-Identifier: Apache-2.0
