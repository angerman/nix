# Regression: removeAttrs with a list of strings whose elements are
# Tag::App entries from map/genList.
#
# Pre-fix (within commit 7adc7e61f) error: `v3 primop removeAttrs:
# expected list of strings` — the names list contained lazy Tag::App
# elements (produced by builtins.map) and the isString check fired
# before the elements were forced.  Fix: force each element to WHNF
# before the isString check.  Mirrors primAttrNames / primCatAttrs
# pattern.
#
# Positive test: this expression must evaluate to the empty attrset.
#
# Run:
#   NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
#     nix eval --impure -f repro-removeAttrs-lazy-elements.nix
#
# Expected output (both TW and v3):
#   { }
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

let
  src = { a = 1; b = 2; c = 3; };
  # The list of names is produced by `map (s: s) [...]` so each element
  # is a Tag::App that must be forced before isString.
  names = builtins.map (s: s) [ "a" "b" "c" ];
in
  builtins.removeAttrs src names
