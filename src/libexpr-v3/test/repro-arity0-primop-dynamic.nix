# Regression test for commit 8c8f883e2:
#   v3 builtins: pre-call arity-0 primops + withLookup arity-0 fallback
#
# Pre-fix: v3 stored arity-0 primops (storeDir, currentSystem,
# nixVersion, langVersion, currentTime, nixPath) as raw Tag::PrimOp
# values in `vBuiltins`.  lower.cc already pre-calls them at STATIC
# `builtins.X` references (lower.cc:826,2861) but DYNAMIC paths
# leaked the raw primop, breaking downstream string coercion.
#
# Failing dynamic paths and TW behaviour:
#   v3 (pre-fix):                        TW / v3 (post-fix):
#   `with builtins; storeDir`  → <PRIMOP>   → "/nix/store"
#   `inherit (builtins) storeDir;` ... .storeDir → <PRIMOP> → "/nix/store"
#   `toString (with builtins; storeDir)` → throw tag=11 → "/nix/store"
#
# This fixture exercises all three patterns plus a few cross-checks
# against `builtins.X` (the static path that was always working).
# v3 and TW must both produce IDENTICAL output post-fix.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0.

let
  # Test A: `with builtins; primop-name` — withLookup arity-0 path.
  withBuiltinsStoreDir = with builtins; storeDir;

  # Test B: `inherit (X) primop;` — attr-select via inherit-from.
  inheritedStoreDir = ({ inherit (builtins) storeDir; }).storeDir;

  # Test C: indirect capture — `let b = builtins; in b.storeDir`.
  capturedStoreDir = (let b = builtins; in b).storeDir;

  # Test D: string coercion of a with-lookup arity-0 primop value.
  # Pre-fix this threw "toString: cannot stringify type tag=11".
  toStringedStoreDir = toString (with builtins; storeDir);

  # Test E: static `builtins.X` (always worked via lower.cc fast-path;
  # included to confirm the post-fix behaviour matches).
  staticStoreDir = builtins.storeDir;

  # Test F: across all arity-0 primops — verify each is a non-PRIMOP
  # value when looked up dynamically.  typeOf of a successful auto-
  # call returns "string" or "list"; for an UN-CALLED Tag::PrimOp it
  # would return "lambda" (which is what TW + post-fix v3 must NOT do
  # for dynamic lookups).
  dynTypes = with builtins; {
    storeDir = builtins.typeOf storeDir;
    currentSystem = builtins.typeOf currentSystem;
    nixVersion = builtins.typeOf nixVersion;
    langVersion = builtins.typeOf langVersion;
  };

in
{
  inherit
    withBuiltinsStoreDir
    inheritedStoreDir
    capturedStoreDir
    toStringedStoreDir
    staticStoreDir
    dynTypes
    ;
}
