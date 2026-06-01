# Regression: native-lower base-env name resolution must match TW.
#
# v3 registers many primops under BARE names (`fetchurl`, `head`, `foldl`,
# `filter`, `genList`, ...) that Nix does NOT expose as base-env globals
# (only `builtins.fetchurl` etc.; bare = undefined variable in TW).  The
# native lowerer resolves a free name by NAME (no TW bindVars), so it must
# only treat a name as a base-env primop when it is a genuine TW global
# (twBaseEnvGlobals) — otherwise it falls through to the `with`-chain, as
# TW's bindVars does.
#
# The bug this guards: bare `fetchurl` shortcutting to `builtins.fetchurl`
# instead of the `with pkgs`-bound nixpkgs FOD — a store-path-affecting
# divergence that produced a `/v3-fake-store/...` firefox.drvPath under
# native-lower while the bridge/TW path produced the real path.
#
# Here `scope` shadows the non-global names; each must resolve to the
# with-binding (a string), NOT the builtin.  `map` IS a genuine global, so
# `builtins.typeOf map` stays "lambda" (and a bare `map` not in the with
# would resolve to the global — lexical/base-env beats `with`).
let
  scope = {
    fetchurl = "with-fetchurl";
    head     = "with-head";
    foldl    = "with-foldl";
    filter   = "with-filter";
    genList  = "with-genList";
  };
in
with scope;
[ fetchurl head foldl filter genList (builtins.typeOf map) ]
