let
  s = {
    a1 = 1;
    a2 = 2;
    a3 = 3;
    a4 = 4;
    a5 = 5;
  };
  p = builtins.unsafeGetAttrPos "a5" s;
in { inherit (p) column line; }