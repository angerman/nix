# Minimal reproducer attempt #3 for the v3 #455 cardano-node failure
# under NIX_V3_LAZY_BRIDGE_ARG=1 + NIX_V3_ON_DEMAND_ROOT=1.
#
# Boils down to: a 3-arg curried lambda whose body uses `with self;`
# over the second arg, evaluated inside a fix-point.  Mirrors aliases.nix's
# `lib: self: super:` shape without the actual nixpkgs/extends machinery.
#
# Trigger conditions:
#   NIX_USE_V3=1 NIX_V3_ON_DEMAND_ROOT=1 [NIX_V3_LAZY_BRIDGE_ARG=1]
#
# Tree-walker baseline + v3 default: produce ["A1" "A2"]
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

let
  fix = f: let x = f x; in x;
  mapAttrs =
    f: attrs:
    builtins.listToAttrs (
      map (n: { name = n; value = f n attrs.${n}; }) (builtins.attrNames attrs)
    );

  # 3-arg curried lambda, mimicking aliases.nix's `lib: self: super:`.
  buildAliases =
    lib: self: super:
    with self;
    let
      checkInPkgs = n: x:
        if builtins.hasAttr n super then throw "${n} also in super" else x;
      removeDistribute = x:
        if lib.isDerivation x then lib.dontDistribute x else x;
    in
    mapAttrs (n: x: removeDistribute (checkInPkgs n x)) {
      one = "1";
      two = "2";
    };

  lib0 = {
    isDerivation = v: v.type or null == "derivation";
    dontDistribute = v: v;
  };

  # The 'self' attrset that fix produces: combines the helpers above
  # with some base attrs so `with self;` has stuff to look up.
  toFix = self: {
    one = "from-self-1";
    two = "from-self-2";
  } // (buildAliases lib0 self {});

  result = fix toFix;
in
[ result.one result.two ]
