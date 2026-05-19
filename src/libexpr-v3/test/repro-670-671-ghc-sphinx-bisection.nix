# Bisection result for #670 (ghc98+ SIGTRAP) and #671 (ghc96 fake-store)
#
# Both issues share the same root cause: the sphinx/docs evaluation
# path inside the Haskell compiler nativeBuildInputs triggers
# nixpkgs's `validatePythonMatches` runtime assertion under v3, which
# under TW does not fire.  Bisection 2026-05-19:
#
#   ghc94.drvPath                                  → OK (no sphinx ref)
#   ghc96.drvPath                                  → /v3-fake-store/...
#   ghc98.drvPath / ghc910 / ghc984                → SIGTRAP exit 133
#
#   ghc96.override{enableDocs=false;}.drvPath      → OK, byte-identical TW
#   ghc98.override{enableDocs=false;}.drvPath      → OK, byte-identical TW
#   ghc910.override{enableDocs=false;}.drvPath     → OK, byte-identical TW
#   ghc984.override{enableDocs=false;}.drvPath     → OK, byte-identical TW
#
# Disabling enableDocs skips sphinx from `nativeBuildInputs` in
# common-hadrian.nix, avoiding the eval path that triggers the
# validatePythonMatches throw inside sphinx's
# `propagatedBuildInputs`.  The actual fix requires v3's callPackage
# fix-point to converge identically to TW (same family as A-series
# bugs).
#
# This fixture serves three purposes:
#   1. Document the workaround for users / packagers.
#   2. Provide a positive regression-guard: if v3 ever regresses on
#      the no-docs path, this catches it.
#   3. Anchor #670/#671 to a smaller, more tractable repro than the
#      full ghc-with-docs eval graph.
{
  ghc94_with_docs    = (import <nixpkgs> {}).haskell.compiler.ghc94.drvPath;
  ghc96_no_docs      = ((import <nixpkgs> {}).haskell.compiler.ghc96.override { enableDocs = false; }).drvPath;
  ghc98_no_docs      = ((import <nixpkgs> {}).haskell.compiler.ghc98.override { enableDocs = false; }).drvPath;
  ghc910_no_docs     = ((import <nixpkgs> {}).haskell.compiler.ghc910.override { enableDocs = false; }).drvPath;
  ghc984_no_docs     = ((import <nixpkgs> {}).haskell.compiler.ghc984.override { enableDocs = false; }).drvPath;
}
