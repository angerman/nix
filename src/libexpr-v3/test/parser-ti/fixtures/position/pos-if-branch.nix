let
  s = if true
      then { t = 1; }
      else { t = 2; };
  p = builtins.unsafeGetAttrPos "t" s;
in { inherit (p) column line; }