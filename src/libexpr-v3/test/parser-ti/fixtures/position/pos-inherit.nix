let
  src = { i = 1; };
  s = {
    inherit (src) i;
  };
  p = builtins.unsafeGetAttrPos "i" s;
in { inherit (p) column line; }