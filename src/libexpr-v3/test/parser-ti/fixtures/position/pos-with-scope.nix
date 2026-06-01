let
  env = { base = { w = 9; }; };
  s = with env; base;
  p = builtins.unsafeGetAttrPos "w" s;
in { inherit (p) column line; }