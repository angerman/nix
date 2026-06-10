<!--
Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
-->
# RCA — haskell.nix / cardano v3-direct spin (2026-06-10)

A SECOND, distinct #455-class non-terminating spin, surfaced by the Lever B M5/HNE bake.
**Independent of Lever B** (8B and the 16B-reference binary — both with the splitString
#455 fix, commit 7beaf0746 — spin IDENTICALLY), and **distinct from the splitString
under-applied-PAP cycle** (that one is fixed; this is a different trigger).

## Symptom

Under pure v3-direct (`NIX_V3_DIRECT_EVAL=1`, the full `nix` CLI), evaluating a
haskell.nix-based flake does NOT terminate, where TW finishes instantly:
- `(getFlake haskell-nix-example).packages.aarch64-darwin.hello.drvPath` — spins
- `(getFlake cardano-node).packages.aarch64-darwin.cardano-node.name` — spins
- even `builtins.attrNames (getFlake haskell-nix-example).packages.aarch64-darwin` spins
  (**TW returns the 7 package names instantly**).

## Characterization — an INFINITE TIGHT LOOP (not over-forcing, not super-linear)

Alloc counters at the wall-cap, `attrNames …packages.aarch64-darwin`:
- 30 s: closures=5612 lists=4252 attrsets=11268 thunks=0
- 60 s: closures=5612 …
- 120 s: closures=5612 lists=4250 attrsets=11227 (RSS 66→49 MB as GC reclaims)

⇒ v3 allocates the haskell.nix/eachSystem **setup (~5612 closures) then PLATEAUS** and
spins in a **non-allocating force/dispatch cycle** over the built structure — the same
*class* as splitString (a force cycle), NOT a runaway allocation and NOT mere slowness.
`thunks=0` throughout.

## Localization

- **Sample @ 8 s:** entirely in `primGetFlake → ffi::lockFlakeAndRead` (ffi.cc:784) — the
  FFI **flake-lock prelude** (locking haskell.nix's large input tree). Finite; not the loop.
- **Sample @ 28 s:** flat across the dispatchLoop prologue (vm.cc:3021–3279: per-op
  threadArena/safepoint) + **OP_FORCE** (vm.cc:7081–7109) + `applyForceWriteback`
  (vm.cc:2429) — a force-heavy busy loop, no single dominant leaf.
- **`V3_DBG_TRAP_ON_LIMIT` @ 45 s** (16 frames, valueStack=143, withStack=1): the v3 eval
  of **flake-utils `eachSystem`** (`eachSystemPassThrough` / `eachSystemOp` /
  `defaultSystems` + `foldl'`, frame[6]) → **`recursiveUpdate` / `recursiveUpdateUntil`**
  (the `isAttrs`×2 + `pred` + `f` merge recursion, frame[4], cu=0xcb31d0dd0 ip≈2734) →
  haskell.nix **`getLib`** (frame[8], `OP_ATTRS_SELECT ; getLib`).

So: after the (finite) flake lock, v3 loops in the **flake-utils `eachSystem` fold +
`recursiveUpdate` merge + haskell.nix `getLib`** path.

- **Deepest frames (14–15)** pin the INNERMOST loop more precisely: haskell.nix's
  **overlay / `extends` application** — `OP_CALL_PRIMOP functionArgs`, `lib.isFunction`,
  `lib.isList`, **`crossOverlays`**, `throwIfNot`, `warn` (overlay-signature inspection +
  applying the large overlay stack via `makeExtensible`/`extends`/`composeExtensions`),
  running on the **register VM** (`OP_R_RETURN`/`OP_R_BRANCH_FALSE`) with
  `OP_GET_UPVALUE_REC_BINDING_SLOT` rec-sibling resolution. ⇒ the cycle is **applying
  haskell.nix's big overlay/extends stack** inside the eachSystem fold — plausibly the same
  **`extends`/APPLY_OVERRIDES over-forcing class** flagged in the elaborate RCA
  (RCA_455_VNATIVE §"What it IS") + the #495 Fix/Extends/Compose intrinsics, now hit at
  haskell.nix scale (a deep overlay chain).

## Hypotheses KILLED (Rule 0)

- **NOT the under-applied-PAP #455** — the 16B-reference binary (c690b3f19) HAS that fix
  and spins identically (~81/95 closures), and 8B≡16B; so this is a different cycle.
- **NOT `attrNames` over-forcing** — `builtins.attrNames { a = throw "X"; b = 1; }` →
  `[ "a" "b" ]` in v3 (matches TW; values stay lazy). `isAttrs { a = throw; }` → true.
- **NOT `recursiveUpdate` over-forcing** — `(recursiveUpdate { a={x=throw;}; } { a={y=42;}; }).a.y`
  → 42 in v3 and `attrNames …a` → `[ "x" "y" ]` (x stays lazy; matches TW).

⇒ the components are lazy/correct in isolation; the trigger needs the **full
haskell.nix/eachSystem structure** — exactly the pattern splitString showed (works in any
standalone shape; only spins as compiled inside the real lib).

## Next (open)

1. **Narrow to a minimal repro** (the hard part): an `eachSystem`-style `foldl'` +
   `recursiveUpdate` over a small per-system attrset that references a sibling/`getLib`-like
   thunk — mirroring the trap's frame[4–6] shape. (Same method as splitString:
   import-a-rec-module + the specific call shape.)
2. **Trap the exact looping op** in cu=0xcb31d0dd0 (recursiveUpdateUntil) + a deep-frame
   sample to see whether it's a force-cycle (OP_FORCE re-entry on a Slot/App), a memo miss
   (re-forcing an un-memoized thunk), or a rec-binding-slot re-resolution.
3. Fix + validate: HNE/M5 complete in v3-direct + lang 142/143 unchanged.

## Repro pointers

- Local: `~/Projects/iohk/haskell-nix-example`; 8B `nix` CLI at `build/src/nix/nix`.
  `NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=30s build/src/nix/nix eval --impure --expr
  'builtins.attrNames (builtins.getFlake "/Users/angerman/Projects/iohk/haskell-nix-example").packages.aarch64-darwin'`
- darwin-4: `~/Projects/iohk/{haskell-nix-example,cardano-node}`; 8B `nix` at
  `~/Projects/iohk/nix/build/src/nix/nix`, 16B-ref at `~/nix16/build16/src/nix/nix`.
