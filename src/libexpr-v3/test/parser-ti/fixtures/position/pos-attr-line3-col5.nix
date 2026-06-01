let
  s = {
    x = 1;
  };
  p = builtins.unsafeGetAttrPos "x" s;
in { inherit (p) column line; }