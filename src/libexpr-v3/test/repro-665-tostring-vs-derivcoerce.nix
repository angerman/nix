# Regression test for #665 (commit landing 2026-05-19):
#   v3: split `toString` (non-copying) from a new `__derivCoerce`
#   (copying) so user-facing `toString ./X` matches TW's
#   coerceToString(copyToStore=false) while drv-attr coercion still
#   matches TW's coerceToString(copyToStore=true).
#
# Pre-fix: v3's `toString` unconditionally copied path values to
# /nix/store. nixpkgs's `substitute.nix` (and `replaceVarsWith.nix`)
# compute the derivation name as `baseNameOf (toString args.src)`.
# With over-aggressive toString, the source path became
# `/nix/store/<hash>-X.sh` and baseNameOf returned `<hash>-X.sh`
# instead of TW's `X.sh` — every wrapped setup-hook derivation
# (sdk-hook.sh, the LLVMgold patch, ...) had a misnamed name attr,
# cascading drv-hash divergence across the entire stdenv tree.
#
# Post-fix: `toString` keeps TW's non-copying semantics; the
# bytecode `derivationStrict` wrapper uses the new internal
# `builtins.__derivCoerce` primop wherever TW would have used
# coerceToString(copyToStore=true) — args / builder / env entries.
#
# Synthetic re-creates the two patterns:
#   Case A — `toString ./path` returning source-tree path (used
#            by `name = baseNameOf (toString args.src)`).
#   Case B — `${./path}` interpolation in `args` array (used by
#            `args = [ "-e" ./builder.sh ]`), which DOES need to be
#            store-copied.
#
# Both cases must produce drvPath byte-identical between TW and v3
# via `nix eval --impure -f <this> + NIX_V3_DIRECT_EVAL=1`.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0.

let
  thisPath = ./repro-665-tostring-vs-derivcoerce.nix;

  # Case A: derivation name derived via baseNameOf (toString src).
  # Must equal "repro-665-tostring-vs-derivcoerce.nix" in both
  # evaluators (TW's toString = source-tree path; baseNameOf strips
  # to "repro-665-tostring-vs-derivcoerce.nix" — NOT
  # "<hash>-repro-665-tostring-vs-derivcoerce.nix").
  caseA = derivation {
    name = baseNameOf (toString thisPath);
    system = "x86_64-linux";
    builder = "/bin/sh";
    args = [ "-c" "true" ];
  };

  # Case B: path interpolated into args.  Must be copied to
  # /nix/store and appear in drv.inputSrcs (TW's
  # coerceToString(copyToStore=true) behavior).  drv.args entry
  # gets the resulting /nix/store/<hash>-... path.
  caseB = derivation {
    name = "path-context-test";
    system = "x86_64-linux";
    builder = "/bin/sh";
    args = [ "-c" "cp ${thisPath} $out" ];
  };

in
{
  inherit (caseA) drvPath;
  caseBDrvPath = caseB.drvPath;
}
