let
  mk = v: { y = v; };
  s = mk 7;
  p = builtins.unsafeGetAttrPos "y" s;
in { inherit (p) column line; }