let
  outer =
    let
      mid = {
        leaf = 1;
      };
    in mid;
  p = builtins.unsafeGetAttrPos "leaf" outer;
in { inherit (p) column line; }