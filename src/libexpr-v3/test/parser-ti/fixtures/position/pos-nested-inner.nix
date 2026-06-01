let
  s = {
    outer = {
      inner = 42;
    };
  };
  p = builtins.unsafeGetAttrPos "inner" s.outer;
in { inherit (p) column line; }