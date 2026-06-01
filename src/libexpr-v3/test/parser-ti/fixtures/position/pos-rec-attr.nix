let
  s = rec {
    a = 1;
    b = a;
  };
  p = builtins.unsafeGetAttrPos "b" s;
in { inherit (p) column line; }