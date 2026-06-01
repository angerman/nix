let
  a = { m = 1; };
  b = {
    m = 2;
  };
  s = a // b;
  p = builtins.unsafeGetAttrPos "m" s;
in { inherit (p) column line; }