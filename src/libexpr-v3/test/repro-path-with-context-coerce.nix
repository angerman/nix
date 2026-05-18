# Regression test for commit 78fa43631:
#   v3: emit __structuredAttrs in env + copy-path-to-store for path coercion
#
# Two fixes coupled into one fixture.
#
# FIX 1 — Path-to-store coercion (primops.cc:toStringCoerceCtx Tag::Path):
# Pre-fix v3 returned `v.payload.path` raw without copying to /nix/store
# or adding string-context.  TW's coerceToString with copyToStore=true
# (eval.cc:2898) copies via copyPathToStore and adds an Opaque context
# entry.  Result: any derivation interpolating a relative path like
# `${./X.sh}` got the SOURCE-TREE path in drv.args instead of the
# /nix/store-COPIED path TW produces — and drv.inputSrcs was empty.
#
# FIX 2 — __structuredAttrs env inclusion (bytecode_primops.cc):
# Pre-fix v3's derivationStrict bytecode wrapper filtered
# `__structuredAttrs` from drv.env via flagKeys.  TW INCLUDES it in
# drv.env (coerced to "" when false) for non-structured derivations.
# Verified by diffing mirrors-list.drv content.
#
# This fixture builds a derivation that uses BOTH patterns:
# - `__structuredAttrs = false` explicitly (must appear as "" in env)
# - A path interpolation in args (must be copied to /nix/store with context)
#
# Pre-fix v3: drvPath differs from TW.
# Post-fix v3: drvPath byte-identical to TW.
#
# Note: this fixture only matches via the production path
# (`nix-instantiate --eval` with NIX_V3_DIRECT_EVAL=1).  Via `nix eval
# --impure` (run-repros style), an additional divergence remains —
# tracked under task #665.  For now, classified SKIP_PARITY in
# run-repros, but a manual nix-instantiate check shows MATCH.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0.

let
  # Use a path with context: any relative path in the source tree.
  # The fixture file ITSELF is convenient — it always exists when this
  # fixture runs.
  pathWithContext = ./repro-path-with-context-coerce.nix;

  drv = derivation {
    name = "path-context-test";
    system = "x86_64-linux";
    builder = "/bin/sh";

    # __structuredAttrs = false explicitly: must appear as "" in env
    # (pre-fix v3 filtered it out; TW includes).
    __structuredAttrs = false;

    # Args contains the path via interpolation: TW copies the file to
    # /nix/store and adds an Opaque context entry; pre-fix v3 used the
    # raw source-tree path and produced empty inputSrcs.
    args = [ "-c" "cp ${pathWithContext} $out" ];
  };

in
  drv.drvPath
