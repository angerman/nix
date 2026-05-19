# Regression test for #666 (commit landing 2026-05-19):
#   v3 valueEqual (in primops.cc, used by primElem / primGenericClosure
#   / etc.) must special-case derivations and compare them by their
#   `outPath` attribute alone — matching TW's eqValues at
#   libexpr/eval.cc:3365.
#
# Pre-fix:
#   - v3's vm.cc::valueEqual (OP_EQ) DID handle derivations specially.
#   - v3's primops.cc::valueEqual (primElem, primGenericClosure, ...)
#     did NOT, so `builtins.elem drv [ samedrv ]` could return false
#     even though `drv == samedrv` returned true.
#
# Result: `lib.unique` (which uses `builtins.elem`) failed to dedup
# derivation references that arrived via different paths (e.g.
# `pkgs.python3` vs `pkgs.python3Packages.python` — both point at the
# same drv but the attrset shapes differ slightly).  Duplicates
# leaked into `requiredPythonModules`, then into the python3-env
# buildEnv's chosenOutputs JSON, then into the structuredAttrs JSON
# the drv hash is computed from — every python3-env derivation
# diverged between v3 and TW.  Cascade: llvm-21.1.8 → clang-21.1.8
# → stdenv-darwin → every package on darwin.
#
# Post-fix: primops.cc:valueEqual mirrors vm.cc:valueEqual — checks
# `type = "derivation"` on both sides, returns outPath-equality if
# so.  `builtins.elem` and `==` now report the same answer.
#
# This fixture exercises both the direct equality and the elem path,
# using a pkgs reference that differs at the attrset level but
# points at the same underlying derivation.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0.

let
  pkgs = import <nixpkgs> { };
  # Two references to the same python3 derivation that arrive via
  # different attrset shapes (pkgs.python3Packages.python passes
  # through a different override chain than pkgs.python3).
  p3 = pkgs.python3;
  p3' = pkgs.python3Packages.python;
in
{
  # `==` already worked even pre-fix (uses vm.cc::valueEqual).
  eq = p3 == p3';

  # `builtins.elem` used primops.cc::valueEqual which lacked the
  # derivation special case pre-fix.  Must agree with `==`.
  elemFwd = builtins.elem p3 [ p3' ];
  elemBwd = builtins.elem p3' [ p3 ];

  # `lib.unique` is `foldl' (acc: e: if elem e acc then acc else
  # acc ++ [e]) []`.  With the elem fix, two refs to the same drv
  # dedupe to one entry.
  uniqLen = builtins.length (pkgs.lib.unique [ p3 p3' p3 ]);
}
