# Repro for #670 (ghc98+ SIGTRAP) and #671 (ghc96 /v3-fake-store/).
#
# Root cause was a DANGLING std::string_view in primops.cc's
# primDerivationStrictNative: the loop captured `key` from the global
# symbol table via std::string_view, then called forceValue and
# valueToJsonWithContext on attr values.  Those calls may intern new
# symbols, which grows std::vector<std::string> and moves every string
# in the table to new storage — invalidating the earlier string_view.
# When the dangling view was later copied into the structuredAttrs
# JSON key map, it picked up garbage bytes from whatever now lived at
# that address.
#
# Symptom: under v3-direct, nixpkgs's `mkPythonPackage`'s
# `validatePythonMatches` assertion fired non-deterministically with
# "Python version mismatch in 'python3.13-sphinx-9.1.0': alabaster's
# pythonModule != sphinx's python", because alabaster's pythonModule
# evaluated to a python3-3.13.12 derivation with a CORRUPTED
# structuredAttrs key — yielding a different drvPath than sphinx's
# python (which had un-corrupted attrs).  The two python3 derivations
# differed ONLY in the structuredAttrs JSON: one key was the
# legitimate "CPPFLAGS" string, the other was 8 garbage bytes left
# over from a moved-from std::string.
#
# Fix (commit TBD): capture the key as a `std::string` (full copy)
# instead of `std::string_view` BEFORE any call that may intern
# symbols.  See primops.cc:6072-area.
#
# This fixture exercises all 5 Haskell compilers that previously hit
# the bug.  Pre-fix: ghc96 produced /v3-fake-store/...; ghc98/910/984
# SIGTRAPped (exit 133); ghc94 worked (no sphinx in nativeBuildInputs).
# Post-fix: all 5 byte-identical to TW.
let
  pkgs = import <nixpkgs> {};
  hcompiler = pkgs.haskell.compiler;
in {
  ghc94  = hcompiler.ghc94.drvPath;
  ghc96  = hcompiler.ghc96.drvPath;
  ghc98  = hcompiler.ghc98.drvPath;
  ghc910 = hcompiler.ghc910.drvPath;
  ghc984 = hcompiler.ghc984.drvPath;
}
