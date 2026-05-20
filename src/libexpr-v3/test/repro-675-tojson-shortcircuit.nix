# Repro for #675 — `nix eval --impure --json` on a derivation attrset
# should emit just the outPath string, not the deep attrset.
#
# Pre-fix: v3's toJsonValue in print.cc lacked TW's `outPath` and
# `__toString` short-circuit (TW: libexpr/value-to-json.cc:52-58).
# v3 instead recursed into every attr of a derivation, exploding into
# the full transitive nixpkgs graph reachable from `src` / `stdenv`
# / etc.  Concretely `(import <nixpkgs> {}).hello.drvAttrs.src` took
# >3 minutes and emitted 0 bytes under v3, vs <2s and one-line
# `"/nix/store/<hash>-hello-2.12.3.tar.gz"` under TW.
#
# Fix (commit TBD): toJsonValue takes a `VMState&` so it can lazy-
# force as it serializes, and short-circuits attrsets with
# `__toString` or `outPath` to match TW's printValueAsJSON.  The
# upfront `forceDeep` call from the CLI was also dropped for --json
# (now only --strict triggers it).
#
# Fixture exercises a derivation with `outPath`, a derivation reached
# via `drvAttrs.src` (the original failing case), a non-derivation
# attrset (must still emit the full object), and a deeply-nested
# graph (proves no perf regression).
let
  pkgs = import <nixpkgs> {};
in {
  hello_src = pkgs.hello.drvAttrs.src;
  hello_stdenv = pkgs.hello.drvAttrs.stdenv;
  hello_drvAttrs = pkgs.hello.drvAttrs;
  # Non-derivation attrset — full object expected.
  plain_attrs = { a = 1; b = 2; c = "x"; };
  # Mixed: attrset of derivations.
  bundle = { hello = pkgs.hello; bash = pkgs.bash; };
  # Nested via mapAttrs.
  paths = builtins.mapAttrs (n: v: v.outPath) { a = pkgs.hello; b = pkgs.bash; };
}
