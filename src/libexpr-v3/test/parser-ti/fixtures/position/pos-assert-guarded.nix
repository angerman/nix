let
  s = assert true;
    { g = 5; };
  p = builtins.unsafeGetAttrPos "g" s;
in { inherit (p) column line; }