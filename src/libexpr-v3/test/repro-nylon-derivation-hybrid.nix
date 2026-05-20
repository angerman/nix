# KNOWN-FAIL synthetic repro for the bytecode `derivation` hybrid wrapper
# triggering infinite-recursion in module-system fix-points.
#
# Pattern from nixpkgs services (nylon.nix family):
#
#   - module has `cfg = config.services.X` in top-level let
#   - uses `lib.filter` / `lib.collect` on cfg
#   - has `config = lib.mkIf <cfg-derived-condition> { ... }`
#
# TW returns `{ }` (default; no services enabled).
# v3 throws "infinite recursion encountered" while evaluating
# `_module.freeformType'.
#
# Bisection:
#   - NIX_V3_NO_BC_DERIVATION_HYBRID=1 → MATCHES TW (workaround)
#   - NIX_V3_NO_BC_DERIV_TOPLEVEL=1 → hangs (different shape; partial install
#     leaves derivation/derivationStrict in inconsistent state)
#   - NIX_V3_NO_BC_DERIV_STRICT=1 → STILL FAILS (top-level wrapper alone
#     causes the eval-time cycle detection)
#   - Both NO_BC_DERIV_TOPLEVEL=1 AND NO_BC_DERIV_STRICT=1 → MATCHES TW
#
# So the bug requires BOTH bytecode wrappers to be installed.  This is an
# INTERACTION bug — installing the derivation hybrid wrapper pair affects
# downstream lazy evaluation in nixpkgs's module system (lib.evalModules)
# even when derivation itself is never called.
#
# Not yet fixed.  Tracked as architectural #455-family work; the deep fix
# needs more investigation than this session has room for.  The workaround
# `NIX_V3_NO_BC_DERIVATION_HYBRID=1` unblocks NixOS module eval at the cost
# of the bytecode wrapper's perf benefits.
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
          # Empty mkIf body — but the mkIf condition eval triggers the cycle.
        };
      })
  ];
}).config.services.foo
