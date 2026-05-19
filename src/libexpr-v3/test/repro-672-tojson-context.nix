# Repro for #672 — builtins.toJSON drops string context
#
# Before fix: v3's primToJSON used valueToJson() which traversed v3
# Values but never propagated string contexts of interpolated paths /
# derivations into the resulting JSON-encoded string.  Tree-walker's
# prim_toJSON uses printValueAsJSON with a NixStringContext accumulator
# and SETS that context on the output string buffer (libexpr/primops.cc:
# 2154-area).
#
# Downstream impact: `lib.generators.toLua` emits `toJSON "${drv}"` for
# each derivation it serializes.  The resulting string LOST every
# derivation context that should have flowed into the writeText output
# — luaPackages.* derivations' configFile.drv had inputDrvs MISSING
# the referenced `lua`, `wrap-lua-hook`, `luarocks_bootstrap` entries.
# Cascade-diverged drvPaths for every Lua package, neovim, etc.
#
# Fix (e26a12812 + this commit): primToJSON now uses
# valueToJsonWithContext + setStringContext on the buffer; the latter
# also gained `__toString` handling so the lang-test
# eval-okay-tojson's `{ __toString = self: self.a; a = "foo"; }` case
# still works.
#
# Expected (matches TW):
#   {"/nix/store/<hash>-lua-5.2.4.drv":{"outputs":["out"]}}
let pkgs = import <nixpkgs> {};
in {
  context_of_interp = builtins.getContext (builtins.toJSON "${pkgs.lua}");
  context_of_attr   = builtins.getContext (builtins.toJSON { a = pkgs.lua; });
  # __toString lookup path still triggers correctly (mirrors lang-test).
  toString_attr = builtins.toJSON { __toString = self: self.a; a = "foo"; };
}
