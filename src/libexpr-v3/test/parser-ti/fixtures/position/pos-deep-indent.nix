let
  s = {
            deep = 1;
  };
  p = builtins.unsafeGetAttrPos "deep" s;
in { inherit (p) column line; }