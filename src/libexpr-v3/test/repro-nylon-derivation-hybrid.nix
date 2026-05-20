# FIXED in 2026-05-20 by #694 (lower.cc isTrivialForLazy whitelist tightening).
#
# Pattern from nixpkgs services (nylon.nix family):
#
#   - module has `cfg = config.services.X` in top-level let
#   - uses `lib.filter` / `lib.attrValues` on cfg
#   - has `config = lib.mkIf <cfg-derived-condition> { ... }` where the
#     condition uses a `<` / `>` operator (parses to ExprCall(__lessThan, ...))
#
# Pre-#694 v3 evaluated `__lessThan` arg-position calls EAGERLY (via
# lower.cc isTrivialForLazy's `__lessThan` whitelist entry), so the
# `mkIf (length attrNames cfg > 0)` cond was forced at mkIf-call time
# rather than thunkified.  Forcing `cfg = config.services.foo` mid-module-
# merge tripped lib.evalModules's `_module.freeformType` cycle that TW
# navigates by treating the cond as a deferred thunk inside `mkIf`'s
# `{ _type="if"; condition = <thunk>; ... }` result.
#
# Operator bisection (2026-05-20):
#   - `length X > 0` / `0 < length X` / `length X < 1` / `1 > length X` → FAILED
#   - `length X >= 1` / `length X != 0` / `length X == 0` → WORKED
#     (these parse to Not(Less{...}) or Eq/NEq, which weren't whitelisted)
#
# Fix: removed `__lessThan` from `isTrivialForLazy(forArg=true)`'s eager
# whitelist.  `__sub`/`__mul`/`__div` stay eager — they're routinely used
# in arithmetic hot paths (`f (n - 1)`) and don't have cycle-vector args.
#
# This file is a positive regression guardrail.  TW returns `{ }` and v3
# now matches.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
let
  pkgs = import <nixpkgs> {};
  lib = pkgs.lib;
in (lib.evalModules {
  modules = [
    ({ config, lib, ... }:
      let
        cfg = config.services.foo;
        enabledItems = lib.filter (p: p.enable == true) (lib.attrValues cfg);
      in {
        options.services.foo = lib.mkOption {
          type = lib.types.attrsOf (lib.types.submodule {
            options.enable = lib.mkOption { type = lib.types.bool; default = false; };
          });
          default = {};
        };
        config = lib.mkIf (builtins.length enabledItems > 0) {
          # Empty mkIf body — pre-fix the mkIf condition eval triggered the
          # `_module.freeformType` cycle.  Post-#694, TW and v3 both return
          # `{ }`.
        };
      })
  ];
}).config.services.foo
