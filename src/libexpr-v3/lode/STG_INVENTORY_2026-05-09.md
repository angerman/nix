# STG-mode inventory — 2026-05-09 (#547 Phase 1)

Goal: catalogue what fails under `NIX_V3_STG=1` so we can fix the
gaps and flip the default.  Inventory runs every workload under five
modes: TW (baseline), v3-direct (current), v3-direct + STG, v3-fhook
(current), v3-fhook + STG.

## Result matrix

```
workload             | tw          | v3-direct | v3-direct-stg | v3-fhook   | v3-fhook-stg
---------------------+-------------+-----------+---------------+------------+-------------
rec-simple           | OK          | OK        | OK            | OK         | OK
rec-self-dot         | OK          | OK        | OK            | OK         | OK
let-prev-update      | OK          | OK        | OK            | OK         | OK
let-rec-mutual       | OK          | OK        | OK            | OK         | OK
formals-default      | OK          | OK        | OK            | OK         | OK
formals-mutual       | OK          | OK        | OK            | OK         | OK
lib-id               | OK          | OK        | OK            | OK         | OK
lib-fix-simple       | OK          | OK        | OK            | OK         | OK
lib-extends-1        | OK          | OK        | OK            | OK         | OK
lib-extends-with     | OK          | OK        | OK            | OK         | OK
lib-makeextensible   | OK          | OK        | OK            | OK         | OK
nixpkgs-typeof       | OK "set"    | CYCLE     | TIMEOUT       | BLACKHOLE  | OK "set"
nixpkgs-lib-id       | OK 42       | CYCLE     | TIMEOUT       | BLACKHOLE  | OK 42
nixpkgs-hello-name   | OK "hello"  | CYCLE     | (≥10s)        | (untested) | (untested)
```

## Reading the matrix

- **All synthetic + lib-only workloads pass under every mode.**  STG
  mode does not regress anything covered by today's slot mechanism +
  cell-update.

- **STG mode FIXES v3-fhook on nixpkgs.**  Without STG, `v3-fhook`
  blackholes on `nixpkgs-typeof` / `nixpkgs-lib-id`.  With STG, it
  matches TW — `OK "set"` and `OK 42`.  This is the canonical legacy
  publish-corruption: the mechanism wrote wrong-shape values onto
  outer Black thunks; turning it off makes v3-fhook fall back to TW
  via the refused hooks, which handles the construction correctly.

- **v3-direct fails on nixpkgs with or without STG.**  This is a
  pre-existing eval-order divergence in v3-direct's pkgs construction
  that surfaces as a same-VM cycle, NOT a publish/cell-update gap.
  STG default-on doesn't make this worse, but doesn't fix it either.

## Strategic implication

Two clearly separable problems:

1. **STG-default-for-v3-fhook (#547 Phase 2): READY.**  Flipping
   `NIX_V3_STG` default-on under v3-fhook is a strict win — fixes
   nixpkgs failures, no regressions on tested workloads.  Path:
   - flip the 6 env-var gates (vm.cc x3, v3_hook.cc x2, lower.cc x1)
     to opt-out instead of opt-in;
   - keep `NIX_V3_NO_STG=1` as escape hatch;
   - delete the legacy `publishToNearestBlackThunkFrame` body once
     the dust settles (next session — touchy because of widespread
     comment references).

2. **v3-direct eval-order divergence on full nixpkgs (#547 Phase 3):
   SEPARATE.**  v3-direct triggers a force inside lib.fix's `x` thunk
   for some inner thunk that has `with pkgs;` in scope, while TW
   never reaches that force during construction.  Diagnosed at frame
   level (CALLPACKAGE_BUG_2026-05-09.md): the trigger is a call to
   `recurseIntoAttrs` (via the all-packages.nix body) that forces
   its `attrs` parameter eagerly.  Pre-existing v3-direct bug — fix
   is independent of STG.

## Test reproducer

The full inventory lives in
`src/libexpr-v3/test/inventory-stg-mode.sh` — runs in ~2 minutes
without nixpkgs, much longer with (full nixpkgs eval × 5 modes).

```sh
TIMEOUT=10 src/libexpr-v3/test/inventory-stg-mode.sh
```

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input
Output Group.
SPDX-License-Identifier: Apache-2.0
